#include "core/server_shutdown.hpp"
#include "config/unified_observable_config.hpp"
#include "database/database_manager.hpp"
#include "core/web/web_server.hpp"
#include "orchestration/thread_pool_manager.hpp"
#include "orchestration/scheduler_service.hpp"
#include "orchestration/files_manager.hpp"
#include "core/console_input_manager.hpp"
#include <Poco/Logger.h>
#include <chrono>

namespace MediaDedup
{
    ServerShutdown::ServerShutdown(std::shared_ptr<UnifiedObservableConfigManager> config_manager)
        : config_manager_(std::move(config_manager))
    {
    }

    void ServerShutdown::handleShutdown(
        std::unique_ptr<DatabaseManager> &database_manager,
        std::unique_ptr<WebServer> &web_server,
        std::shared_ptr<ThreadPoolManager> &tpm,
        std::shared_ptr<Orchestration::SchedulerService> &scheduler_service,
        std::shared_ptr<Orchestration::FilesManager> &files_manager,
        std::shared_ptr<MediaProcessor> &media_processor,
        ::MediaDedupServer::Core::ConsoleInputManager &console_input_manager,
        size_t console_subscription_id)
    {
        Poco::Logger &logger = Poco::Logger::get("ServerShutdown");

        logger.information("Shutting down server...");

        // Stop console input processing
        try
        {
            logger.debug("Stopping console input processing...");
            if (console_subscription_id > 0)
            {
                console_input_manager.unsubscribeFromConsoleEvents(console_subscription_id);
                logger.debug("Unsubscribed from console events, ID: %zu", console_subscription_id);
            }
            console_input_manager.stop();
            logger.debug("Console input manager stopped successfully");
        }
        catch (const std::exception &e)
        {
            logger.error("Exception during console input shutdown: %s", e.what());
        }
        catch (...)
        {
            logger.error("Unknown exception during console input shutdown");
        }

        // Stop scheduler service
        try
        {
            if (scheduler_service)
            {
                logger.debug("Stopping scheduler service...");
                scheduler_service->stop();
                logger.debug("Scheduler service stopped successfully");
            }
            else
            {
                logger.debug("Scheduler service is null, skipping");
            }
        }
        catch (const std::exception &e)
        {
            logger.error("Exception during scheduler service shutdown: %s", e.what());
        }
        catch (...)
        {
            logger.error("Unknown exception during scheduler service shutdown");
        }

        // Stop MediaProcessor
        try
        {
            if (media_processor)
            {
                logger.debug("Shutting down media processor...");
                media_processor->shutdown();
                logger.debug("Media processor shutdown complete");
            }
            else
            {
                logger.debug("Media processor is null, skipping");
            }
        }
        catch (const std::exception &e)
        {
            logger.error("Exception during media processor shutdown: %s", e.what());
        }
        catch (...)
        {
            logger.error("Unknown exception during media processor shutdown");
        }

        // Stop TPM
        try
        {
            if (tpm)
            {
                logger.debug("Shutting down thread pool manager...");
                tpm->shutdownAndDrain(std::chrono::milliseconds(10000));
                logger.debug("Thread pool manager shutdown complete");
            }
            else
            {
                logger.debug("Thread pool manager is null, skipping");
            }
        }
        catch (const std::exception &e)
        {
            logger.error("Exception during thread pool manager shutdown: %s", e.what());
        }
        catch (...)
        {
            logger.error("Unknown exception during thread pool manager shutdown");
        }

        // Stop web server
        try
        {
            if (web_server)
            {
                logger.debug("Stopping web server...");
                web_server->stop();
                logger.debug("Web server stopped successfully");
            }
            else
            {
                logger.debug("Web server is null, skipping");
            }
        }
        catch (const std::exception &e)
        {
            logger.error("Exception during web server shutdown: %s", e.what());
        }
        catch (...)
        {
            logger.error("Unknown exception during web server shutdown");
        }

        // Cleanup database
        try
        {
            if (database_manager)
            {
                logger.debug("Cleaning up database manager...");
                // Database cleanup is handled automatically by unique_ptr destructor
                logger.debug("Database manager cleanup complete");
            }
            else
            {
                logger.debug("Database manager is null, skipping");
            }
        }
        catch (const std::exception &e)
        {
            logger.error("Exception during database cleanup: %s", e.what());
        }
        catch (...)
        {
            logger.error("Unknown exception during database cleanup");
        }

        logger.information("Server shutdown complete");

        // Cleanup static instances to prevent abort during global destructor phase
        try
        {
            logger.debug("Cleaning up static instances...");
            ::MediaDedupServer::Core::ConsoleInputManager::cleanupStaticInstance();
            logger.debug("Static instances cleanup complete");
        }
        catch (const std::exception &e)
        {
            logger.error("Exception during static instances cleanup: %s", e.what());
        }
        catch (...)
        {
            logger.error("Unknown exception during static instances cleanup");
        }
    }

    void ServerShutdown::logShutdownInfo()
    {
        Poco::Logger &logger = Poco::Logger::get("ServerShutdown");

        logger.information("=== Media Deduplication Server Shutdown ===");
        logger.information("Server has been stopped gracefully");
    }
}
