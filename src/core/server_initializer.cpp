#include "core/server_initializer.hpp"
#include "config/unified_observable_config.hpp"
#include "database/database_manager.hpp"
#include "database/database_service.hpp"
#include "database/user_settings_ops.hpp"
#include "database/user_settings_service.hpp"
#include "core/web_server.hpp"
#include "filesmanager/files_service.hpp"
#include "orchestration/thread_pool_manager.hpp"
#include "orchestration/scheduler_service.hpp"
#include "orchestration/files_manager.hpp"
#include <Poco/Logger.h>
#include <filesystem>

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
            // Get server configuration
            std::string host = config_manager_->getPropertyValue<std::string>("server.host", "0.0.0.0");
            int port = config_manager_->getPropertyValue<int>("server.port", 8080);

            // Create web server
            web_server_ = std::make_unique<WebServer>(config_manager_);
            web_server_->setHost(host);
            web_server_->setPort(static_cast<uint16_t>(port));

            // Setup request handlers
            setupRequestHandlers();

            // Start web server
            if (!web_server_->start())
            {
                logger.error("Failed to start web server");
                return false;
            }

            logger.information("Web server started on %s:%d", host, port);
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

            // Initialize FilesManager
            auto db_shared = std::shared_ptr<DatabaseManager>(database_manager_.get(), [](DatabaseManager *) {});
            auto files_service = std::make_shared<FilesService>(*db_shared);
            files_manager_ = std::make_shared<Orchestration::FilesManager>(config_manager_, db_shared, files_service);
            files_manager_->initialize();

            // Register fileScan job
            int intervalMs = config_manager_->getPropertyValue<int>("files.manager.scan.intervalMs", 300000);
            logger.information("Loading fileScan interval from config: %d ms", intervalMs);
            scheduler_service_->registerJob("fileScan", std::chrono::milliseconds(intervalMs), "fileScan",
                                            [fm = files_manager_]()
                                            { fm->runOnce(); });

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
