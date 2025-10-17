#include "core/server_initializer.hpp"
#include "config/unified_observable_config.hpp"
#include "database/database_manager.hpp"
#include "database/database_service.hpp"
#include "database/user_settings_ops.hpp"
#include "database/user_settings_service.hpp"
#include "database/scanned_files_service.hpp"
#include "database/thumbnail_cache_ops.hpp"
#include "core/web/web_server.hpp"
#include "filesmanager/files_service.hpp"
#include "filesmanager/disk_cache.hpp"
#include "orchestration/thread_pool_manager.hpp"
#include "orchestration/scheduler_service.hpp"
#include "orchestration/files_manager.hpp"
#include "orchestration/duplicate_finder.hpp"
#include "media_processors/media_processor.hpp"
#include "media_processors/image/backends/onnx_adapter.hpp"
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

            // Note: Thread stack size optimization is configured in ThreadPoolManager
            // HTTP server threads will use system default (8MB) - this is acceptable for HTTP processing
            // TPM threads will use optimized stack size (1MB) via tpm.thread.stackSizeKB setting

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

            // Initialize database manager with config manager for reactive pool sizing
            database_manager_ = std::make_unique<DatabaseManager>(db_path, config_manager_);
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

            // Set duplicate finder on web server for status endpoint
            if (duplicate_finder_)
            {
                web_server_->setDuplicateFinder(duplicate_finder_);
                logger.information("DuplicateFinder set on web server");
            }
            else
            {
                logger.warning("DuplicateFinder is null - not setting on web server");
            }

            // Set database manager and thumbnail cache on web server for thumbnail endpoint
            auto db_shared = std::shared_ptr<DatabaseManager>(database_manager_.get(), [](DatabaseManager *) {});
            if (db_shared)
            {
                web_server_->setDatabaseManager(db_shared);
                logger.information("DatabaseManager set on web server");
            }

            if (thumbnail_disk_cache_)
            {
                web_server_->setThumbnailCache(thumbnail_disk_cache_);
                logger.information("ThumbnailCache set on web server");
            }
            else
            {
                logger.warning("ThumbnailCache is null - not setting on web server");
            }

            // Set transcoding cache on web server for thumbnail API
            // Reuse the same disk cache as media processor for transcoding
            auto transcoding_cache = std::make_shared<DiskCache>(config_manager_, "cache.disk");
            if (transcoding_cache->initialize())
            {
                web_server_->setTranscodingCache(transcoding_cache);
                logger.information("TranscodingCache set on web server for thumbnail API");
            }
            else
            {
                logger.warning("Failed to initialize transcoding cache for thumbnail API");
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

            // Initialize thumbnail disk cache with separate configuration
            thumbnail_disk_cache_ = std::make_shared<DiskCache>(config_manager_, "cache.thumbnail");
            if (!thumbnail_disk_cache_->initialize())
            {
                logger.error("Failed to initialize thumbnail disk cache");
                return false;
            }
            logger.information("Thumbnail disk cache initialized successfully");

            // Ensure thumbnail_cache database table exists
            if (!ThumbnailCacheOps::ensureTable(*database_manager_))
            {
                logger.error("Failed to ensure thumbnail_cache table");
                return false;
            }
            logger.information("Thumbnail cache table ensured");

            // Initialize ONNX Session Manager (for QUALITY mode memory optimization)
            // Must be initialized before MediaProcessor for session reuse
            OnnxAdapter::initializeSessionManager(config_manager_);

            // Initialize MediaProcessor first (needed by FilesManager)
            auto db_shared = std::shared_ptr<DatabaseManager>(database_manager_.get(), [](DatabaseManager *) {});
            media_processor_ = std::make_shared<MediaProcessor>(config_manager_, db_shared, tpm_);
            if (!media_processor_->initialize())
            {
                logger.error("Failed to initialize MediaProcessor");
                return false;
            }
            logger.information("MediaProcessor initialized successfully");

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

            // Initialize and register duplicate finder job
            duplicate_finder_ = std::shared_ptr<Orchestration::DuplicateFinder>(
                new Orchestration::DuplicateFinder(config_manager_, *database_manager_));
            if (!duplicate_finder_->initialize())
            {
                logger.error("Failed to initialize DuplicateFinder");
                return false;
            }
            logger.information("DuplicateFinder initialized successfully");

            int dupIntervalMs = config_manager_->getPropertyValue<int>("duplicates.finder.intervalMs", 3600000);
            logger.information("Loading duplicateFinder interval from config: %d ms (%d hours)",
                               dupIntervalMs, dupIntervalMs / 3600000);
            scheduler_service_->registerJob("duplicateFinder", std::chrono::milliseconds(dupIntervalMs), "duplicate_finder",
                                            [df = duplicate_finder_]()
                                            { df->findDuplicates(); });

            // Subscribe to duplicate finder interval changes
            config_manager_->subscribeToConfigChanges([weakScheduler = std::weak_ptr<Orchestration::SchedulerService>(scheduler_service_),
                                                       cfg = config_manager_](const ConfigChangeEvent &event)
                                                      {
                if (event.key == "duplicates.finder.intervalMs")
                {
                    auto scheduler = weakScheduler.lock();
                    if (!scheduler)
                    {
                        return;
                    }

                    int newIntervalMs = cfg->getPropertyValue<int>("duplicates.finder.intervalMs", 3600000);
                    Poco::Logger &logger = Poco::Logger::get("ServerInitializer");
                    logger.information("Duplicate finder interval changed to: %d ms (%d hours)",
                                      newIntervalMs, newIntervalMs / 3600000);
                    
                    scheduler->updateJobInterval("duplicateFinder", std::chrono::milliseconds(newIntervalMs));
                } });

            // Set up immediate job triggering callback for FilesService
            files_service_->setPathRegisteredCallback([weakScheduler = std::weak_ptr<Orchestration::SchedulerService>(scheduler_service_),
                                                       cfg = config_manager_, this](const std::string &directory_path)
                                                      {
                Poco::Logger &logger = Poco::Logger::get("ServerInitializer");
                logger.debug("Path registered, triggering immediate scan and process: %s", directory_path);

                // Submit immediate scan task to thread pool instead of creating detached thread
                // This prevents thread accumulation and ensures proper lifecycle management
                if (tpm_)
                {
                    logger.debug("Submitting immediate scan task to thread pool for: %s", directory_path);
                    tpm_->submit("fileScan", [weakScheduler, cfg, directory_path]() {
                        Poco::Logger &threadLogger = Poco::Logger::get("ServerInitializer");
                        threadLogger.debug("Starting immediate scan task for: %s", directory_path);

                        auto scheduler = weakScheduler.lock();
                        if (!scheduler)
                        {
                            threadLogger.debug("Scheduler no longer available, skipping immediate scan and process");
                            return;
                        }

                        // Check if scheduler is still running before attempting to trigger jobs
                        if (!scheduler->isRunning()) {
                            threadLogger.debug("Scheduler is not running, skipping immediate scan and process");
                            return;
                        }

                        // Step 1: Trigger fileScan
                        bool scanTriggered = scheduler->triggerJob("fileScan");
                        if (!scanTriggered) {
                            threadLogger.debug("fileScan job was already running or not found, skipping immediate scan");
                            return;
                        }

                        // Step 2: Wait for fileScan to complete (with timeout)
                        auto start = std::chrono::steady_clock::now();
                        int timeoutMs = cfg->getPropertyValue<int>("files.service.immediateJobTrigger.timeoutMs", 30000);
                        auto timeout = std::chrono::milliseconds(timeoutMs);

                        while (scheduler->isJobRunning("fileScan") && scheduler->isRunning()) {
                            if (std::chrono::steady_clock::now() - start > timeout) {
                                threadLogger.warning("fileScan job did not complete within timeout, skipping immediate processing");
                                return;
                            }
                            std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Poll every 100ms
                        }

                        // Check if scheduler is still running before triggering mediaProcessor
                        if (!scheduler->isRunning()) {
                            threadLogger.debug("Scheduler stopped during fileScan wait, skipping immediate processing");
                            return;
                        }

                        // Step 3: Trigger mediaProcessor
                        bool processTriggered = scheduler->triggerJob("mediaProcessor");
                        if (processTriggered) {
                            threadLogger.debug("mediaProcessor job triggered successfully");
                        } else {
                            threadLogger.debug("mediaProcessor job was already running or not found");
                        }

                        threadLogger.debug("Immediate scan task completed for: %s", directory_path);
                    });
                    logger.debug("Immediate scan task submitted to thread pool for: %s", directory_path);
                }
                else
                {
                    logger.warning("ThreadPoolManager not available, skipping immediate scan for: %s", directory_path);
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
