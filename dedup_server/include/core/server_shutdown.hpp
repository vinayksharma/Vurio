#pragma once

#include <memory>
#include <string>
#include "core/console_input_manager.hpp"

namespace MediaDedup
{
    class UnifiedObservableConfigManager;
    class DatabaseManager;
    class WebServer;
    class ThreadPoolManager;
    class MediaProcessor;
    // Forward declaration for ConsoleInputManager in different namespace
    namespace Core
    {
        class ConsoleInputManager;
    }

    namespace Orchestration
    {
        class SchedulerService;
        class FilesManager;
    }

    /**
     * @brief Handles server shutdown logic
     */
    class ServerShutdown
    {
    public:
        /**
         * @brief Constructor
         * @param config_manager Configuration manager instance
         */
        explicit ServerShutdown(std::shared_ptr<UnifiedObservableConfigManager> config_manager);

        /**
         * @brief Destructor
         */
        ~ServerShutdown() = default;

        /**
         * @brief Handle graceful shutdown
         * @param database_manager Database manager instance
         * @param web_server Web server instance
         * @param tpm Thread pool manager instance
         * @param scheduler_service Scheduler service instance
         * @param files_manager Files manager instance
         * @param media_processor Media processor instance
         * @param console_input_manager Console input manager instance
         * @param console_subscription_id Console subscription ID
         */
        void handleShutdown(
            std::unique_ptr<DatabaseManager> &database_manager,
            std::unique_ptr<WebServer> &web_server,
            std::shared_ptr<ThreadPoolManager> &tpm,
            std::shared_ptr<Orchestration::SchedulerService> &scheduler_service,
            std::shared_ptr<Orchestration::FilesManager> &files_manager,
            std::shared_ptr<MediaProcessor> &media_processor,
            ::MediaDedupServer::Core::ConsoleInputManager &console_input_manager,
            size_t console_subscription_id);

        /**
         * @brief Log server shutdown information
         */
        void logShutdownInfo();

    private:
        std::shared_ptr<UnifiedObservableConfigManager> config_manager_;
    };
}
