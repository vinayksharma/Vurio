#include "core/server_shutdown.hpp"
#include "config/unified_observable_config.hpp"
#include "database/database_manager.hpp"
#include "core/web_server.hpp"
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
            ::MediaDedupServer::Core::ConsoleInputManager &console_input_manager,
            size_t console_subscription_id
        )
        {
            Poco::Logger &logger = Poco::Logger::get("ServerShutdown");
            
            logger.information("Shutting down server...");

            // Stop console input processing
            if (console_subscription_id > 0)
            {
                console_input_manager.unsubscribeFromConsoleEvents(console_subscription_id);
            }
            console_input_manager.stop();

            // Stop scheduler service
            if (scheduler_service)
            {
                scheduler_service->stop();
            }

            // Stop TPM
            if (tpm)
            {
                tpm->shutdownAndDrain(std::chrono::milliseconds(10000));
            }

            // Stop web server
            if (web_server)
            {
                web_server->stop();
            }

            // Cleanup database
            if (database_manager)
            {
                // Database cleanup is handled automatically by unique_ptr destructor
            }

            logger.information("Server shutdown complete");
        }

        void ServerShutdown::logShutdownInfo()
        {
            Poco::Logger &logger = Poco::Logger::get("ServerShutdown");
            
            logger.information("=== Media Deduplication Server Shutdown ===");
            logger.information("Server has been stopped gracefully");
    }
}
