#include "core/server_initializer.hpp"
#include "config/unified_observable_config.hpp"
#include "database/database_manager.hpp"
#include "database/database_service.hpp"
#include "database/user_settings_ops.hpp"
#include "database/user_settings_service.hpp"
#include "database/scanned_files_service.hpp"
#include "core/web/web_server.hpp"
#include "filesmanager/files_service.hpp"
#include "orchestration/thread_pool_manager.hpp"
#include "orchestration/scheduler_service.hpp"
#include "orchestration/files_manager.hpp"
#include "media_processors/media_processor.hpp"
#include <Poco/Logger.h>
#include <filesystem>
#include <thread>
#include <chrono>

namespace MediaDedup
{
    ServerInitializer::ServerInitializer(std::shared_ptr<UnifiedObservableConfigManager> config_manager)
        : config_manager_(std::move(config_manager))
    {
    }

    bool ServerInitializer::initializeConfiguration()
    {
        Poco::Logger &logger = Poco::Logger::get("ServerInitializer");

        try
        {
            // Initialize the configuration manager
            if (!config_manager_->initialize())
            {
                logger.error("Failed to initialize configuration manager");
                return false;
            }

            logger.information("Configuration initialized successfully");
            return true;
        }
        catch (const std::exception &e)
        {
            logger.error("Failed to load configuration: %s", e.what());
            return false;
        }
    }

    bool ServerInitializer::initializeDatabase()
    {
        Poco::Logger &logger = Poco::Logger::get("ServerInitializer");

        try
        {
            // Get database path from config
            std::string db_path = config_manager_->getPropertyValue<std::string>("database.path", "data/dedup_server.db");

            // Create database directory if it doesn't exist
            std::filesystem::path db_dir = std::filesystem::path(db_path).parent_path();
            if (!db_dir.empty() && !std::filesystem::exists(db_dir))
            {
                std::filesystem::create_directories(db_dir);
            }

            // Initialize database manager
            database_manager_ = std::make_unique<DatabaseManager>(db_path);
            if (!database_manager_->initialize())
            {
                logger.error("Failed to initialize database manager");
                return false;
            }

            // Initialize UserSettingsService
            user_settings_service_ = std::make_shared<UserSettingsService>(*database_manager_);
            if (!user_settings_service_->initialize())
            {
                logger.error("Failed to initialize user settings service");
                return false;
            }

            logger.information("Database initialized successfully");
            return true;
        }
        catch (const std::exception &e)
        {
            logger.error("Failed to initialize database: %s", e.what());
            return false;
        }
    }

    bool ServerInitializer::initializeWebServer()
    {
        Poco::Logger &logger = Poco::Logger::get("ServerInitializer");

        try
        {
            // Get host and port from config
            std::string host = config_manager_->getPropertyValue<std::string>("server.host", "0.0.0.0");
            int port_int = config_manager_->getPropertyValue<int>("server.port", 8080);
            uint16_t port = static_cast<uint16_t>(port_int);

            // Create web server with scheduler service
            web_server_ = std::make_unique<WebServer>(config_manager_, host, port, scheduler_service_);

            // Set up services
            if (user_settings_service_)
            {
                web_server_->setUserSettingsService(user_settings_service_);
                logger.information("UserSettingsService set on web server");
            }
            else
            {
                logger.warning("UserSettingsService is null - not setting on web server");
            }

            if (files_service_)
            {
                web_server_->setFilesService(files_service_);
                logger.information("FilesService set on web server");
            }
            else
            {
                logger.warning("FilesService is null - not setting on web server");
            }

            if (scanned_files_service_)
            {
                web_server_->setScannedFilesService(scanned_files_service_);
                logger.information("ScannedFilesService set on web server");
            }
            else
            {
                logger.warning("ScannedFilesService is null - not setting on web server");
            }

            if (tpm_)
            {
                web_server_->setThreadPoolManager(tpm_);
                logger.information("ThreadPoolManager set on web server");
            }
            else
            {
                logger.warning("ThreadPoolManager is null - not setting on web server");
            }

            // Start the web server
            if (!web_server_->start())
            {
                logger.error("Failed to start web server");
                return false;
            }

            logger.information("Web server started on %s:%u", host, static_cast<unsigned int>(port));
            return true;
        }
        catch (const std::exception &e)
        {
            logger.error("Failed to initialize web server: %s", e.what());
            return false;
        }
    }

    bool ServerInitializer::initializeTPM()
    {
        Poco::Logger &logger = Poco::Logger::get("ServerInitializer");

        try
        {
            // Initialize TPM
            tpm_ = std::make_shared<ThreadPoolManager>(config_manager_);
            tpm_->initialize();

            logger.information("Thread Pool Manager initialized successfully");
            return true;
        }
        catch (const std::exception &e)
        {
            logger.error("Failed to initialize TPM: %s", e.what());
            return false;
        }
    }

    bool ServerInitializer::initializeSchedulerAndFiles()
    {
        Poco::Logger &logger = Poco::Logger::get("ServerInitializer");

        try
        {
            // Initialize SchedulerService
            scheduler_service_ = std::make_shared<Orchestration::SchedulerService>(config_manager_, tpm_);
            scheduler_service_->start();

            // Initialize MediaProcessor first (needed by FilesManager)
            auto db_shared = std::shared_ptr<DatabaseManager>(database_manager_.get(), [](DatabaseManager *) {});
            media_processor_ = std::make_shared<MediaProcessor>(config_manager_, db_shared, tpm_);
            media_processor_->initialize();

            // Initialize FilesManager with MediaProcessor
            files_service_ = std::make_shared<FilesService>(*db_shared, config_manager_);
            scanned_files_service_ = std::make_shared<ScannedFilesService>(*db_shared);
            files_manager_ = std::make_shared<Orchestration::FilesManager>(config_manager_, db_shared, files_service_, media_processor_);
            files_manager_->initialize();

            // Register fileScan job
            int intervalMs = config_manager_->getPropertyValue<int>("files.manager.scan.intervalMs", 300000);
            logger.information("Loading fileScan interval from config: %d ms", intervalMs);
            scheduler_service_->registerJob("fileScan", std::chrono::milliseconds(intervalMs), "fileScan",
                                            [fm = files_manager_]()
                                            { fm->runOnce(); });

            // Clear any processing flags from interrupted sessions before starting media processing
            logger.information("Clearing processing flags from any interrupted sessions...");
            int cleared_count = media_processor_->clearProcessingFlags();
            if (cleared_count > 0)
            {
                logger.information("Cleared processing flags for %d files that were left in processing state", cleared_count);
            }
            else if (cleared_count == 0)
            {
                logger.information("No files were left in processing state");
            }
            else
            {
                logger.warning("Failed to clear processing flags - database operation failed");
            }

            // Register mediaProcessor job
            int mediaIntervalMs = config_manager_->getPropertyValue<int>("media.processor.intervalMs", 30000);
            logger.information("Loading mediaProcessor interval from config: %d ms", mediaIntervalMs);
            scheduler_service_->registerJob("mediaProcessor", std::chrono::milliseconds(mediaIntervalMs), "fileScan",
                                            [mp = media_processor_]()
                                            { mp->ProcessMedia(); });

            // Set up immediate job triggering callback for FilesService
            files_service_->setPathRegisteredCallback([this](const std::string &directory_path)
                                                      {
                Poco::Logger &logger = Poco::Logger::get("ServerInitializer");
                logger.debug("Path registered, triggering immediate scan and process: %s", directory_path);
                
                // Check if scheduler is still running before attempting to trigger jobs
                if (!scheduler_service_->isRunning()) {
                    logger.debug("Scheduler is not running, skipping immediate scan and process");
                    return;
                }
                
                // Step 1: Trigger fileScan
                bool scanTriggered = scheduler_service_->triggerJob("fileScan");
                if (!scanTriggered) {
                    logger.debug("fileScan job was already running or not found, skipping immediate scan");
                    return;
                }
                
                // Step 2: Wait for fileScan to complete (with timeout)
                auto start = std::chrono::steady_clock::now();
                int timeoutMs = config_manager_->getPropertyValue<int>("files.service.immediateJobTrigger.timeoutMs", 30000);
                auto timeout = std::chrono::milliseconds(timeoutMs);
                
                while (scheduler_service_->isJobRunning("fileScan") && scheduler_service_->isRunning()) {
                    if (std::chrono::steady_clock::now() - start > timeout) {
                        logger.warning("fileScan job did not complete within timeout, skipping immediate processing");
                        return;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Poll every 100ms
                }
                
                // Check if scheduler is still running before triggering mediaProcessor
                if (!scheduler_service_->isRunning()) {
                    logger.debug("Scheduler stopped during fileScan wait, skipping immediate processing");
                    return;
                }
                
                // Step 3: Trigger mediaProcessor
                bool processTriggered = scheduler_service_->triggerJob("mediaProcessor");
                if (processTriggered) {
                    logger.debug("mediaProcessor job triggered successfully");
                } else {
                    logger.debug("mediaProcessor job was already running or not found");
                } });

            logger.information("Scheduler and Files Manager initialized successfully");
            return true;
        }
        catch (const std::exception &e)
        {
            logger.error("Failed to initialize scheduler and files: %s", e.what());
            return false;
        }
    }

    void ServerInitializer::setupConfigChangeCallback(std::function<void(const std::string &)> callback)
    {
        config_manager_->setFileChangeCallback(callback);
    }

    void ServerInitializer::setupRequestHandlers()
    {
        // This would be implemented based on the existing web server setup
        // For now, we'll leave it as a placeholder since the web server
        // handles its own request handler setup
    }
}
