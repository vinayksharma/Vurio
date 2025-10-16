#pragma once

#include <memory>
#include <string>
#include <functional>

namespace MediaDedup
{
    class UnifiedObservableConfigManager;
    class DatabaseManager;
    class WebServer;
    class ThreadPoolManager;
    class ConsoleInputManager;
    class MediaProcessor;
    class DiskCache;

    namespace Orchestration
    {
        class SchedulerService;
        class FilesManager;
        class DuplicateFinder;
    }

    /**
     * @brief Handles server initialization logic
     */
    class ServerInitializer
    {
    public:
        /**
         * @brief Constructor
         * @param config_manager Configuration manager instance
         */
        explicit ServerInitializer(std::shared_ptr<UnifiedObservableConfigManager> config_manager);

        /**
         * @brief Destructor
         */
        ~ServerInitializer() = default;

        /**
         * @brief Initialize configuration
         * @return true if successful, false otherwise
         */
        bool initializeConfiguration();

        /**
         * @brief Initialize database
         * @return true if successful, false otherwise
         */
        bool initializeDatabase();

        /**
         * @brief Initialize web server
         * @return true if successful, false otherwise
         */
        bool initializeWebServer();

        /**
         * @brief Initialize TPM (Thread Pool Manager)
         * @return true if successful, false otherwise
         */
        bool initializeTPM();

        /**
         * @brief Initialize scheduler service and files manager
         * @return true if successful, false otherwise
         */
        bool initializeSchedulerAndFiles();

        /**
         * @brief Setup configuration change callback
         * @param callback Function to call when configuration changes
         */
        void setupConfigChangeCallback(std::function<void(const std::string &)> callback);

        // Getters for initialized components
        std::shared_ptr<UnifiedObservableConfigManager> getConfigManager() const { return config_manager_; }
        std::unique_ptr<DatabaseManager> &getDatabaseManager() { return database_manager_; }
        std::unique_ptr<WebServer> &getWebServer() { return web_server_; }
        std::shared_ptr<ThreadPoolManager> &getTPM() { return tpm_; }
        std::shared_ptr<Orchestration::SchedulerService> &getSchedulerService() { return scheduler_service_; }
        std::shared_ptr<Orchestration::FilesManager> &getFilesManager() { return files_manager_; }
        std::shared_ptr<MediaProcessor> &getMediaProcessor() { return media_processor_; }
        std::shared_ptr<Orchestration::DuplicateFinder> &getDuplicateFinder() { return duplicate_finder_; }

    private:
        std::shared_ptr<UnifiedObservableConfigManager> config_manager_;
        std::unique_ptr<DatabaseManager> database_manager_;
        std::unique_ptr<WebServer> web_server_;
        std::shared_ptr<ThreadPoolManager> tpm_;
        std::shared_ptr<Orchestration::SchedulerService> scheduler_service_;
        std::shared_ptr<Orchestration::FilesManager> files_manager_;
        std::shared_ptr<Orchestration::DuplicateFinder> duplicate_finder_;
        std::shared_ptr<class UserSettingsService> user_settings_service_;
        std::shared_ptr<class FilesService> files_service_;
        std::shared_ptr<class ScannedFilesService> scanned_files_service_;
        std::shared_ptr<class MediaProcessor> media_processor_;
        std::shared_ptr<class DiskCache> thumbnail_disk_cache_;

        /**
         * @brief Setup request handlers for web server
         */
        void setupRequestHandlers();
    };
}
