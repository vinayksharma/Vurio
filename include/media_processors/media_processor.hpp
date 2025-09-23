#pragma once

#include <string>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <filesystem>
#include "config/unified_observable_config.hpp"
#include "config/config_change_event.hpp"
#include "config/config_enums.hpp" // For ServerMode

namespace MediaDedup
{
    // Forward declarations
    class DatabaseManager;
    struct ScannedFileRow;
    /**
     * @brief Media processor router that determines processing strategy based on file type and server mode
     *
     * This class provides thread-safe routing of media files to appropriate processors
     * based on file extension and current server configuration.
     */
    class MediaProcessor
    {
    public:
        /**
         * @brief Constructor
         * @param config_manager Shared pointer to the unified configuration manager
         * @param database_manager Shared pointer to the database manager
         */
        explicit MediaProcessor(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                                std::shared_ptr<DatabaseManager> database_manager);

        /**
         * @brief Destructor
         */
        ~MediaProcessor();

        /**
         * @brief Route a media file to the appropriate processor based on file type and server mode
         *
         * This method is thread-safe and will:
         * 1. Extract file extension and determine file type category
         * 2. Check if the file type is supported and enabled in configuration
         * 3. Determine processing mode from server.mode configuration
         * 4. Route to appropriate processor (currently ImageProcessor)
         *
         * @param file_path Fully qualified path to the media file
         * @return true if file was successfully routed for processing, false if unsupported or disabled
         */
        bool RouteToProcessor(const std::string &file_path);

        /**
         * @brief Process all unprocessed media files based on current server mode
         *
         * This method is thread-safe and will:
         * 1. Query database for unprocessed files in current server mode
         * 2. Mark files as in-progress (-1) in the database
         * 3. Route each file to appropriate processor via RouteToProcessor
         * 4. Mark files as processed (1) on success or log errors and continue
         *
         * This method processes all unprocessed files in a single run.
         */
        void ProcessMedia();

        /**
         * @brief Internal version of RouteToProcessor that doesn't acquire the mutex
         *
         * This is used by ProcessMedia to avoid deadlock since ProcessMedia already holds the mutex.
         *
         * @param file_path Fully qualified path to the media file
         * @return true if file was successfully routed for processing, false if unsupported or disabled
         */
        bool RouteToProcessorInternal(const std::string &file_path);

        /**
         * @brief Initialize the processor and subscribe to configuration changes
         * @return true if initialization successful, false otherwise
         */
        bool initialize();

        /**
         * @brief Shutdown the processor and unsubscribe from configuration changes
         */
        void shutdown();

    private:
        // Configuration management
        std::shared_ptr<UnifiedObservableConfigManager> config_manager_;

        // Database management
        std::shared_ptr<DatabaseManager> database_manager_;

        // Thread safety
        mutable std::mutex route_mutex_;

        // File type mapping
        std::unordered_map<std::string, std::string> extension_to_config_key_;

        // Configuration change callback
        void onConfigChange(const ConfigChangeEvent &event);

        // Helper methods
        std::string extractFileExtension(const std::string &file_path) const;
        std::string getConfigKeyForExtension(const std::string &extension) const;
        bool isFileTypeSupported(const std::string &config_key) const;
        ServerMode getCurrentServerMode() const;
        void initializeExtensionMapping();

        // File type categories
        enum class FileTypeCategory
        {
            IMAGE,
            VIDEO,
            AUDIO,
            UNSUPPORTED
        };

        FileTypeCategory getFileTypeCategory(const std::string &extension) const;
    };
}
