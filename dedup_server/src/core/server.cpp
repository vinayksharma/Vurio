/*
 * File: core/server.cpp
 * Purpose: Implements the main server application (MediaDedupServer).
 * Summary:
 *   - Parses CLI options, initializes configuration and subsystems
 *   - Starts HTTP web server and orchestrated services (DB, TPM, scheduler)
 *   - Subscribes to console events and handles graceful shutdown
 * Usage:
 *   - Linked into the media_dedup_server executable (see CMakeLists.txt)
 *   - Constructed by Poco::Util::ServerApplication runtime in main
 */
#include "core/server.hpp"
#include "core/logging_setup.hpp"
#include "core/instance_checker.hpp"
#include "core/config_manager.hpp"
#include "core/server_initializer.hpp"
#include "core/server_shutdown.hpp"
#include "core/console_input_manager.hpp"
#include <Poco/Logger.h>
#include <Poco/Util/HelpFormatter.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <cstdlib>

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
        logger_.information("Initializing Media Deduplication Server");
    }

    void MediaDedupServer::uninitialize()
    {
        // Avoid logging here to prevent races with logging subsystem teardown
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
        // Set up color-coded logging with timestamps and log levels
        LoggingSetup logging_setup;
        logging_setup.setupColorLogging();

        // Set initial log level (will be updated after config is loaded)
        Poco::Logger::root().setLevel(Poco::Message::PRIO_INFORMATION);

        logger_.information("Media Deduplication Server starting...");

        if (help_requested_)
        {
            return Application::EXIT_OK;
        }

        try
        {
            // Initialize configuration
            config_manager_ = std::make_shared<UnifiedObservableConfigManager>("config/config.yaml");
            ServerInitializer initializer(config_manager_);

            if (!initializer.initializeConfiguration())
            {
                logger_.error("Failed to initialize configuration");
                return Application::EXIT_CONFIG;
            }

            // Check for existing instances after configuration is loaded
            InstanceChecker instance_checker(config_manager_);
            if (!instance_checker.checkForExistingInstances())
            {
                logger_.error("Another instance of Media Deduplication Server is already running. Exiting.");
                return Application::EXIT_TEMPFAIL;
            }

            // Initialize database
            if (!initializer.initializeDatabase())
            {
                logger_.error("Failed to initialize database");
                return Application::EXIT_CONFIG;
            }

            // Initialize console input manager
            if (!console_input_manager_.initialize())
            {
                logger_.error("Failed to initialize console input manager");
                return Application::EXIT_CONFIG;
            }

            // Initialize TPM
            if (!initializer.initializeTPM())
            {
                logger_.error("Failed to initialize TPM");
                return Application::EXIT_CONFIG;
            }

            // Initialize SchedulerService and FilesManager
            if (!initializer.initializeSchedulerAndFiles())
            {
                logger_.error("Failed to initialize scheduler and files");
                return Application::EXIT_CONFIG;
            }

            // Initialize and start web server (after all services are ready)
            if (!initializer.initializeWebServer())
            {
                logger_.error("Failed to initialize web server");
                return Application::EXIT_CONFIG;
            }

            // Setup configuration change callback
            initializer.setupConfigChangeCallback([this, &initializer](const std::string &file_path)
                                                  {
                try {
                    // Handle configuration changes
                    std::string new_host = config_manager_->getPropertyValue<std::string>("server.host", server_host_);
                    int new_port_i = config_manager_->getPropertyValue<int>("server.port", static_cast<int>(server_port_));
                    uint16_t new_port = static_cast<uint16_t>(new_port_i);
                    std::string new_log_level = config_manager_->getPropertyValue<std::string>("logging.level", logging_level_);
                    int new_acquire_timeout = config_manager_->getPropertyValue<int>("database.session.acquireTimeoutMs", 3000);
                    int new_backoff = config_manager_->getPropertyValue<int>("database.session.acquireBackoffMs", 50);
                    std::string new_mode = config_manager_->getPropertyValue<std::string>("server.mode", server_mode_);

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
                        // Log the change before applying the new level to ensure it's visible
                        logger_.information("Logging level changed: " + logging_level_ + " -> " + new_log_level);
                        logging_level_ = new_log_level;
                        LoggingSetup logging_setup;
                        logging_setup.applyLogLevel(logging_level_);
                        // Log confirmation after applying the new level
                        logger_.information("Logging level successfully updated to: " + new_log_level);
                    }

                    // Apply DB session timings live
                    if (database_manager_)
                    {
                        database_manager_->setSessionAcquireTimeoutMs(new_acquire_timeout);
                        database_manager_->setSessionAcquireBackoffMs(new_backoff);
                    }

                    if (new_mode != server_mode_)
                    {
                        logger_.information("Server mode changed: " + server_mode_ + " -> " + new_mode);
                        server_mode_ = new_mode;
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

            // Get initialized components
            database_manager_ = std::move(initializer.getDatabaseManager());
            web_server_ = std::move(initializer.getWebServer());
            tpm_ = initializer.getTPM();
            scheduler_service_ = initializer.getSchedulerService();
            files_manager_ = initializer.getFilesManager();
            media_processor_ = initializer.getMediaProcessor();

            // Set shutdown callback for scheduler service
            if (scheduler_service_)
            {
                scheduler_service_->setShutdownCallback([this]()
                                                        {
                    Poco::Logger &logger = Poco::Logger::get("MediaDedupServer");
                    logger.error("Scheduler service triggered server shutdown due to job failures");
                    console_input_manager_.stop(); });
            }

            // Initialize server variables for logging
            server_host_ = config_manager_->getPropertyValue<std::string>("server.host", "0.0.0.0");
            server_port_ = static_cast<uint16_t>(config_manager_->getPropertyValue<int>("server.port", 8080));
            database_path_ = config_manager_->getPropertyValue<std::string>("database.path", "data/dedup_server.db");
            logging_level_ = config_manager_->getPropertyValue<std::string>("logging.level", "info");
            server_mode_ = "EMBEDDING"; // Single mode operation

            // Apply the logging level from configuration
            std::cout << "DEBUG: About to apply logging level: " << logging_level_ << std::endl;
            LoggingSetup logging_setup;
            logging_setup.applyLogLevel(logging_level_);
            std::cout << "DEBUG: Logging level applied successfully" << std::endl;

            // Start console input processing AFTER configuration is loaded
            console_input_manager_.start();

            // Log startup information
            logStartupInfo();

            // Wait for shutdown signal
            try
            {
                logger_.debug("Waiting for shutdown signal...");
                waitForShutdown();
                logger_.debug("Shutdown signal received");
            }
            catch (const std::exception &e)
            {
                logger_.error("Exception while waiting for shutdown: %s", e.what());
            }
            catch (...)
            {
                logger_.error("Unknown exception while waiting for shutdown");
            }

            // Handle shutdown gracefully
            try
            {
                logger_.debug("Starting graceful shutdown sequence...");
                ServerShutdown shutdown_handler(config_manager_);
                shutdown_handler.handleShutdown(
                    database_manager_,
                    web_server_,
                    tpm_,
                    scheduler_service_,
                    files_manager_,
                    media_processor_,
                    console_input_manager_,
                    console_subscription_id_);
                logger_.debug("Graceful shutdown completed successfully");
            }
            catch (const std::exception &e)
            {
                logger_.error("Exception during graceful shutdown: %s", e.what());
            }
            catch (...)
            {
                logger_.error("Unknown exception during graceful shutdown");
            }

            // Ensure services are destroyed before application uninitialization / static teardown
            scheduler_service_.reset();
            files_manager_.reset();
            media_processor_.reset();
            tpm_.reset();
            web_server_.reset();
            database_manager_.reset();

            // Hard-exit to bypass global/static destructors in third-party libs that may abort
            std::_Exit(Application::EXIT_OK);
        }
        catch (const std::exception &e)
        {
            logger_.error("Server error: %s", e.what());
            return Application::EXIT_SOFTWARE;
        }
    }

    void MediaDedupServer::waitForShutdown()
    {
        try
        {
            logger_.debug("Waiting for console input manager to finish...");
            // Wait for console input manager to finish
            console_input_manager_.waitForShutdown();
            logger_.debug("Console input manager finished");
        }
        catch (const std::exception &e)
        {
            logger_.error("Exception while waiting for console input manager: %s", e.what());
        }
        catch (...)
        {
            logger_.error("Unknown exception while waiting for console input manager");
        }
    }

    void MediaDedupServer::handleConsoleEvent(const ::MediaDedupServer::Core::ConsoleEvent &event)
    {
        using namespace ::MediaDedupServer::Core;

        try
        {
            switch (event.type)
            {
            case ConsoleEventType::SIGNAL_INTERRUPT:
            case ConsoleEventType::SIGNAL_TERMINATE:
            case ConsoleEventType::SIGNAL_QUIT:
                logger_.information("Received shutdown signal: {}", event.command);
                // Stop the console input manager to trigger shutdown
                try
                {
                    console_input_manager_.stop();
                    logger_.debug("Console input manager stopped successfully");
                }
                catch (const std::exception &e)
                {
                    logger_.error("Exception stopping console input manager: %s", e.what());
                }
                catch (...)
                {
                    logger_.error("Unknown exception stopping console input manager");
                }
                break;

            case ConsoleEventType::COMMAND_EXIT:
            case ConsoleEventType::COMMAND_QUIT:
            case ConsoleEventType::COMMAND_SHUTDOWN:
                logger_.information("Received shutdown command: {}", event.command);
                // Stop the console input manager to trigger shutdown
                try
                {
                    console_input_manager_.stop();
                    logger_.debug("Console input manager stopped successfully");
                }
                catch (const std::exception &e)
                {
                    logger_.error("Exception stopping console input manager: %s", e.what());
                }
                catch (...)
                {
                    logger_.error("Unknown exception stopping console input manager");
                }
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
        catch (const std::exception &e)
        {
            logger_.error("Exception in console event handler: %s", e.what());
        }
        catch (...)
        {
            logger_.error("Unknown exception in console event handler");
        }
    }

    void MediaDedupServer::logStartupInfo()
    {
        logger_.information("=== Media Deduplication Server Started ===");
        logger_.information("Configuration file: config/config.yaml");
        logger_.information("Database path: " + database_path_);
        logger_.information("Web server: " + server_host_ + ":" + std::to_string(server_port_));
        logger_.information("API Docs: http://" + server_host_ + ":" + std::to_string(server_port_) + "/ (Swagger UI)");
        logger_.information("==========================================");
        logger_.information("Server running. Press Ctrl+C to stop or type 'exit' to quit...");
    }

    void MediaDedupServer::logShutdownInfo()
    {
        logger_.information("=== Media Deduplication Server Shutdown ===");
        logger_.information("Server has been stopped gracefully");
    }

    MediaDedupServer::~MediaDedupServer()
    {
        // Destructor implementation - can be extended if needed
    }

} // namespace MediaDedup
