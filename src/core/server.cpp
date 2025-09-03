#include "core/server.hpp"
#include "config/unified_observable_config.hpp"
#include "database/database_manager.hpp"
#include <Poco/Logger.h>
#include <Poco/Util/HelpFormatter.h>
#include <iostream>
#include <filesystem>

namespace MediaDedup
{

    MediaDedupServer::MediaDedupServer()
        : logger_(Poco::Logger::get("MediaDedupServer")), help_requested_(false), daemon_mode_(false), server_port_(8080), server_host_("0.0.0.0")
    {
    }

    void MediaDedupServer::initialize(Application &self)
    {
        // TODO: Implement initialization
        logger_.information("Initializing Media Deduplication Server");
    }

    void MediaDedupServer::uninitialize()
    {
        // TODO: Implement cleanup
        logger_.information("Uninitializing Media Deduplication Server");
    }

    void MediaDedupServer::defineOptions(Poco::Util::OptionSet &options)
    {
        // TODO: Implement command line options
        options.addOption(
            Poco::Util::Option("help", "h", "Display help information")
                .required(false)
                .repeatable(false)
                .callback(Poco::Util::OptionCallback<MediaDedupServer>(this, &MediaDedupServer::handleHelp)));
    }

    void MediaDedupServer::handleOption(const std::string &name, const std::string &value)
    {
        // TODO: Implement option handling
    }

    void MediaDedupServer::handleHelp(const std::string &name, const std::string &value)
    {
        help_requested_ = true;
        Poco::Util::HelpFormatter helpFormatter(options());
        helpFormatter.setCommand(commandName());
        helpFormatter.setUsage("OPTIONS");
        helpFormatter.setHeader("Media Deduplication Server");
        helpFormatter.format(std::cout);
        stopOptionsProcessing();
    }

    int MediaDedupServer::main(const std::vector<std::string> &args)
    {
        logger_.information("Media Deduplication Server starting...");

        if (help_requested_)
        {
            return Application::EXIT_OK;
        }

        try
        {
            // Initialize configuration
            if (!initializeConfiguration())
            {
                logger_.error("Failed to initialize configuration");
                return Application::EXIT_CONFIG;
            }

            // Initialize database
            if (!initializeDatabase())
            {
                logger_.error("Failed to initialize database");
                return Application::EXIT_CONFIG;
            }

            // Initialize and start web server
            if (!initializeWebServer())
            {
                logger_.error("Failed to initialize web server");
                return Application::EXIT_CONFIG;
            }

            // Log startup information
            logStartupInfo();

            // Wait for shutdown signal
            waitForShutdown();

            // Handle shutdown gracefully
            handleShutdown();

            return Application::EXIT_OK;
        }
        catch (const std::exception &e)
        {
            logger_.error("Server error: " + std::string(e.what()));
            return Application::EXIT_SOFTWARE;
        }
    }

    bool MediaDedupServer::initializeConfiguration()
    {
        try
        {
            // Set default config file path if not specified
            if (config_file_.empty())
            {
                config_file_ = "config/config.yaml";
            }

            // Create configuration manager
            config_manager_ = std::make_shared<UnifiedObservableConfigManager>(
                config_file_,
                true,                           // Enable file monitoring
                std::chrono::milliseconds(1000) // 1 second reload interval
            );

            // Initialize configuration
            if (!config_manager_->initialize())
            {
                logger_.error("Failed to initialize configuration manager");
                return false;
            }

            // Create some default configuration properties
            config_manager_->createProperty("server.host", server_host_, "Server host address");
            config_manager_->createProperty("server.port", server_port_, "Server port number");
            config_manager_->createProperty("server.name", "Media Deduplication Server", "Server name");
            config_manager_->createProperty("database.path", database_path_, "Database file path");
            config_manager_->createProperty("logging.level", std::string("info"), "Logging level");

            // Save initial configuration
            if (!config_manager_->triggerSave())
            {
                logger_.warning("Failed to save initial configuration");
            }

            logger_.information("Configuration initialized successfully");
            return true;
        }
        catch (const std::exception &e)
        {
            logger_.error("Configuration initialization failed: " + std::string(e.what()));
            return false;
        }
    }

    bool MediaDedupServer::initializeDatabase()
    {
        try
        {
            // Set default database path if not specified
            if (database_path_.empty())
            {
                database_path_ = "data/dedup_server.db";
            }

            // Create database directory if it doesn't exist
            auto db_dir = std::filesystem::path(database_path_).parent_path();
            if (!db_dir.empty() && !std::filesystem::exists(db_dir))
            {
                std::filesystem::create_directories(db_dir);
            }

            // Create database manager
            database_manager_ = std::make_unique<DatabaseManager>(database_path_);

            // Initialize database
            if (!database_manager_->initialize())
            {
                logger_.error("Failed to initialize database manager");
                return false;
            }

            logger_.information("Database initialized successfully");
            return true;
        }
        catch (const std::exception &e)
        {
            logger_.error("Database initialization failed: " + std::string(e.what()));
            return false;
        }
    }

    bool MediaDedupServer::initializeWebServer()
    {
        try
        {
            // Get server configuration from config manager
            server_host_ = config_manager_->getPropertyValue<std::string>("server.host", server_host_);
            server_port_ = config_manager_->getPropertyValue<uint16_t>("server.port", server_port_);

            // Create web server
            web_server_ = std::make_unique<WebServer>(config_manager_, server_host_, server_port_);

            // Start web server
            if (!web_server_->start())
            {
                logger_.error("Failed to start web server");
                return false;
            }

            logger_.information("Web server started on " + server_host_ + ":" + std::to_string(server_port_));
            return true;
        }
        catch (const std::exception &e)
        {
            logger_.error("Web server initialization failed: " + std::string(e.what()));
            return false;
        }
    }

    void MediaDedupServer::waitForShutdown()
    {
        logger_.information("Server running. Press Ctrl+C to stop...");

        // Simple shutdown wait - in production, you might want to use signals
        while (true)
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    void MediaDedupServer::handleShutdown()
    {
        logger_.information("Shutting down server...");

        if (web_server_)
        {
            web_server_->stop();
        }

        if (config_manager_)
        {
            config_manager_->shutdown();
        }

        logger_.information("Server shutdown complete");
    }

    void MediaDedupServer::logStartupInfo()
    {
        logger_.information("=== Media Deduplication Server Started ===");
        logger_.information("Configuration file: " + config_file_);
        logger_.information("Database path: " + database_path_);
        logger_.information("Web server: " + server_host_ + ":" + std::to_string(server_port_));
        logger_.information("API endpoints:");
        logger_.information("  GET  /api/v1/config - Get all configuration");
        logger_.information("  GET  /api/v1/config/{key} - Get specific property");
        logger_.information("  PUT  /api/v1/config/{key} - Update property");
        logger_.information("  POST /api/v1/config/reload - Reload configuration");
        logger_.information("  GET  /api/v1/config/status - Get system status");
        logger_.information("  GET  /api/openapi.json - OpenAPI specification");
        logger_.information("==========================================");
    }

    void MediaDedupServer::logShutdownInfo()
    {
        logger_.information("=== Media Deduplication Server Shutdown ===");
        logger_.information("Shutdown complete");
        logger_.information("==========================================");
    }

} // namespace MediaDedup
