#include "core/server.hpp"
#include "config/unified_observable_config.hpp"
#include "database/database_manager.hpp"
#include "database/database_service.hpp"
#include "database/user_settings_ops.hpp"
#include "database/user_settings_service.hpp"
#include "core/console_input_manager.hpp"
#include <Poco/Logger.h>
#include <Poco/Util/HelpFormatter.h>
#include <iostream>
#include <filesystem>
#include <thread>
#include <chrono>

namespace MediaDedup
{

    MediaDedupServer::MediaDedupServer()
        : logger_(Poco::Logger::get("MediaDedupServer")),
          console_input_manager_(::MediaDedupServer::Core::ConsoleInputManager::getInstance()),
          console_subscription_id_(0),
          help_requested_(false),
          daemon_mode_(false)
    {
    }

    void MediaDedupServer::initialize(Application &self)
    {
        // Initialization handled in main()
        logger_.information("Initializing Media Deduplication Server");
    }

    void MediaDedupServer::uninitialize()
    {
        // Cleanup handled in handleShutdown()
        logger_.information("Uninitializing Media Deduplication Server");
    }

    void MediaDedupServer::applyDefaultConfigValues()
    {
        // Keep all default config-related values in one place
        if (server_host_.empty())
        {
            server_host_ = "0.0.0.0";
        }
        if (server_port_ == 0)
        {
            server_port_ = 8080;
        }
        if (database_path_.empty())
        {
            database_path_ = "data/dedup_server.db";
        }
        if (logging_level_.empty())
        {
            logging_level_ = "info";
        }
    }

    void MediaDedupServer::applyLogLevel(const std::string &level)
    {
        std::string lowered = level;
        for (char &c : lowered)
            c = static_cast<char>(::tolower(c));

        // Map synonyms to Poco names
        std::string pocoLevel = lowered;
        if (lowered == "info")
            pocoLevel = "information";
        else if (lowered == "warn")
            pocoLevel = "warning";

        try
        {
            logger_.setLevel(pocoLevel);
            Poco::Logger::root().setLevel(pocoLevel);
        }
        catch (...)
        {
            logger_.setLevel("information");
            Poco::Logger::root().setLevel("information");
        }
    }

    void MediaDedupServer::defineOptions(Poco::Util::OptionSet &options)
    {
        // Basic help option
        options.addOption(
            Poco::Util::Option("help", "h", "Display help information")
                .required(false)
                .repeatable(false)
                .callback(Poco::Util::OptionCallback<MediaDedupServer>(this, &MediaDedupServer::handleHelp)));
    }

    void MediaDedupServer::handleOption(const std::string &name, const std::string &value)
    {
        // No-op for now
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

            // Initialize console input manager
            if (!console_input_manager_.initialize())
            {
                logger_.error("Failed to initialize console input manager");
                return Application::EXIT_CONFIG;
            }

            // React to configuration file changes: restart web server if host/port changed, apply log level
            config_manager_->setFileChangeCallback([this](const std::string &file_path)
                                                   {
                try {
                    std::string new_host = config_manager_->getPropertyValue<std::string>("server.host", server_host_);
                    int new_port_i = config_manager_->getPropertyValue<int>("server.port", static_cast<int>(server_port_));
                    uint16_t new_port = static_cast<uint16_t>(new_port_i);
                    std::string new_log_level = config_manager_->getPropertyValue<std::string>("logging.level", logging_level_);

                    if (new_host != server_host_ || new_port != server_port_) {
                        logger_.information("Configuration change detected (host/port). Restarting web server...");
                        logger_.information("Old: " + server_host_ + ":" + std::to_string(server_port_) +
                                            " -> New: " + new_host + ":" + std::to_string(new_port));

                        server_host_ = new_host;
                        server_port_ = new_port;
                        if (web_server_) {
                            web_server_->setHost(server_host_);
                            web_server_->setPort(server_port_);
                            web_server_->restart();
                        }
                    }

                    if (new_log_level != logging_level_)
                    {
                        logger_.information("Logging level changed: " + logging_level_ + " -> " + new_log_level);
                        logging_level_ = new_log_level;
                        applyLogLevel(logging_level_);
                    }
                } catch (const std::exception &e) {
                    logger_.warning(std::string("Failed to handle file change: ") + e.what());
                } });

            // Subscribe to console events
            console_subscription_id_ = console_input_manager_.subscribeToConsoleEvents(
                [this](const ::MediaDedupServer::Core::ConsoleEvent &event)
                {
                    handleConsoleEvent(event);
                });

            // Start console input processing
            console_input_manager_.start();

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
                // Prefer config/config.yaml; if not present, fallback to ./config.yaml
                const std::string config_in_folder = "config/config.yaml";
                const std::string config_in_root = "config.yaml";

                if (std::filesystem::exists(config_in_folder))
                {
                    config_file_ = config_in_folder;
                }
                else if (std::filesystem::exists(config_in_root))
                {
                    config_file_ = config_in_root;
                }
                else
                {
                    // Neither exists; keep the folder path so the manager can create defaults
                    config_file_ = config_in_folder;
                }
            }

            // Track whether a config file existed prior to startup
            bool config_file_existed = std::filesystem::exists(config_file_);

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

            // Ensure consistent defaults before creating properties
            applyDefaultConfigValues();

            // Seed defaults only if missing; otherwise read existing values
            bool created_any_property = false;
            if (!config_manager_->hasProperty("server.host"))
            {
                config_manager_->createProperty("server.host", server_host_, "Server host address");
                created_any_property = true;
            }
            else
            {
                server_host_ = config_manager_->getPropertyValue<std::string>("server.host", server_host_);
            }

            if (!config_manager_->hasProperty("server.port"))
            {
                // Store port as int in config to avoid type-mismatch reads
                config_manager_->createProperty("server.port", static_cast<int>(server_port_), "Server port number");
                created_any_property = true;
            }
            else
            {
                // Read as int to be tolerant of YAML integer typing, then cast
                int port_val = config_manager_->getPropertyValue<int>("server.port", static_cast<int>(server_port_));
                server_port_ = static_cast<uint16_t>(port_val);
            }

            if (!config_manager_->hasProperty("server.name"))
            {
                config_manager_->createProperty("server.name", std::string("Media Deduplication Server"), "Server name");
                created_any_property = true;
            }

            if (!config_manager_->hasProperty("database.path"))
            {
                config_manager_->createProperty("database.path", database_path_, "Database file path");
                created_any_property = true;
            }
            else
            {
                database_path_ = config_manager_->getPropertyValue<std::string>("database.path", database_path_);
            }

            if (!config_manager_->hasProperty("logging.level"))
            {
                config_manager_->createProperty("logging.level", std::string("info"), "Logging level");
                created_any_property = true;
            }
            else
            {
                logging_level_ = config_manager_->getPropertyValue<std::string>("logging.level", logging_level_);
                applyLogLevel(logging_level_);
            }

            // Save only if config did not exist or we created any properties
            if (!config_file_existed || created_any_property)
            {
                if (!config_manager_->triggerSave())
                {
                    logger_.warning("Failed to save initial configuration");
                }
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

            // Ensure DB file exists using database service
            {
                auto db_service = std::make_unique<DatabaseService>(config_manager_);
                if (!db_service->ensureDatabaseFileExists())
                {
                    logger_.error("Failed to ensure database file exists");
                    return false;
                }
                database_path_ = db_service->getDatabasePath();
            }

            // Create database manager (connection/session)
            database_manager_ = std::make_unique<DatabaseManager>(database_path_);

            // Initialize database
            if (!database_manager_->initialize())
            {
                logger_.error("Failed to initialize database manager");
                return false;
            }

            // Ensure core tables exist (user_settings for now)
            if (!UserSettingsOps::ensureTable(*database_manager_))
            {
                logger_.error("Failed to ensure user_settings table exists");
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
            // Get server configuration from config manager (read port as int to avoid any_cast mismatch)
            server_host_ = config_manager_->getPropertyValue<std::string>("server.host", server_host_);
            {
                int port_val = config_manager_->getPropertyValue<int>("server.port", static_cast<int>(server_port_));
                server_port_ = static_cast<uint16_t>(port_val);
            }

            // Create web server
            web_server_ = std::make_unique<WebServer>(config_manager_, server_host_, server_port_);

            // Inject services
            {
                // Share DatabaseManager to service
                auto db_shared = std::shared_ptr<DatabaseManager>(database_manager_.get(), [](DatabaseManager *) {});
                auto user_settings_service = std::make_shared<UserSettingsService>(*db_shared);
                if (!user_settings_service->initialize())
                {
                    logger_.error("Failed to initialize UserSettingsService");
                    return false;
                }
                web_server_->setUserSettingsService(user_settings_service);
            }
            {
                auto db_shared = std::shared_ptr<DatabaseManager>(database_manager_.get(), [](DatabaseManager *) {});
                auto files_service = std::make_shared<FilesService>(*db_shared);
                web_server_->setFilesService(files_service);
            }

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
        logger_.information("Server running. Press Ctrl+C to stop or type 'exit' to quit...");

        // Wait for console input manager to finish
        console_input_manager_.waitForShutdown();
    }

    void MediaDedupServer::handleShutdown()
    {
        logger_.information("Shutting down server...");

        // Stop console input manager
        console_input_manager_.stop();

        // Unsubscribe from console events
        if (console_subscription_id_ > 0)
        {
            console_input_manager_.unsubscribeFromConsoleEvents(console_subscription_id_);
        }

        // Shutdown console input manager
        console_input_manager_.shutdown();

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
        // Prefer localhost for clickable links when binding to 0.0.0.0
        std::string display_host = server_host_.empty() || server_host_ == "0.0.0.0" ? "localhost" : server_host_;
        logger_.information("Web server: " + server_host_ + ":" + std::to_string(server_port_));
        logger_.information("OpenAPI: http://" + display_host + ":" + std::to_string(server_port_) + "/api/openapi.json");
        logger_.information("==========================================");
    }

    void MediaDedupServer::logShutdownInfo()
    {
        logger_.information("=== Media Deduplication Server Shutdown ===");
        logger_.information("Shutdown complete");
        logger_.information("==========================================");
    }

    void MediaDedupServer::handleConsoleEvent(const ::MediaDedupServer::Core::ConsoleEvent &event)
    {
        using namespace ::MediaDedupServer::Core;

        switch (event.type)
        {
        case ConsoleEventType::SIGNAL_INTERRUPT:
        case ConsoleEventType::SIGNAL_TERMINATE:
        case ConsoleEventType::SIGNAL_QUIT:
            logger_.information("Received shutdown signal: {}", event.command);
            // Stop the console input manager to trigger shutdown
            console_input_manager_.stop();
            break;

        case ConsoleEventType::COMMAND_EXIT:
        case ConsoleEventType::COMMAND_QUIT:
        case ConsoleEventType::COMMAND_SHUTDOWN:
            logger_.information("Received shutdown command: {}", event.command);
            // Stop the console input manager to trigger shutdown
            console_input_manager_.stop();
            break;

        case ConsoleEventType::COMMAND_RESTART:
            logger_.information("Received restart command");
            if (web_server_)
            {
                logger_.information("Restarting web server...");
                web_server_->restart();
            }
            break;

        case ConsoleEventType::COMMAND_STATUS:
            logger_.information("Server status requested");
            logStartupInfo();
            break;

        case ConsoleEventType::COMMAND_HELP:
            logger_.information("Available commands:");
            logger_.information("  exit, quit, shutdown - Stop the server");
            logger_.information("  restart - Restart the web server");
            logger_.information("  status - Show server status");
            logger_.information("  help - Show this help message");
            break;

        case ConsoleEventType::UNKNOWN_COMMAND:
            logger_.warning("Unknown command: {}", event.command);
            logger_.information("Type 'help' for available commands");
            break;
        }
    }

} // namespace MediaDedup
