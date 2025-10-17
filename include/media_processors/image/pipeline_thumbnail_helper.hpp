#pragma once

#include <string>
#include <memory>

namespace MediaDedup
{
    class DatabaseManager;
    class DiskCache;
    class UnifiedObservableConfigManager;

    /**
     * @brief Helper for generating thumbnails during image processing pipeline
     *
     * This module provides thread-safe thumbnail generation that runs after
     * artifact generation in the processing pipeline. It reuses the same
     * thumbnail infrastructure as the HTTP API but is designed for batch
     * processing scenarios.
     *
     * Thread Safety:
     * - Safe to call from multiple TPM worker threads concurrently
     * - Uses thread-safe ThumbnailGenerator, DiskCache, and database operations
     * - No shared state between calls
     *
     * Key Features:
     * - Configurable enable/disable
     * - Skips generation if thumbnail already exists (configurable)
     * - Non-blocking: failures don't affect pipeline success
     * - Proper logging and error handling
     */
    class PipelineThumbnailHelper
    {
    public:
        /**
         * @brief Generate a thumbnail for a processed image file
         *
         * This method is called at the end of each pipeline (Fast/Balanced/Quality)
         * to pre-generate thumbnails for the web UI. It checks configuration,
         * existing thumbnails, and generates if needed.
         *
         * @param file_path Original file path of the processed image
         * @param db Database manager for thumbnail cache operations
         * @param thumbnail_cache Disk cache for storing thumbnail files
         * @param config_manager Configuration manager for runtime settings
         * @return true if thumbnail was generated or already exists, false on error
         *         (but pipeline processing continues regardless)
         *
         * Thread-safe: Can be called concurrently from multiple TPM threads
         */
        static bool generateThumbnail(const std::string &file_path,
                                      DatabaseManager &db,
                                      DiskCache &thumbnail_cache,
                                      UnifiedObservableConfigManager &config_manager);

    private:
        /**
         * @brief Check if a valid thumbnail already exists for the file
         *
         * @param file_path Original file path
         * @param size Thumbnail size to check
         * @param db Database manager
         * @return true if valid cached thumbnail exists, false otherwise
         */
        static bool thumbnailExists(const std::string &file_path,
                                    int size,
                                    DatabaseManager &db);

        /**
         * @brief Get the modified time of a file
         *
         * @param file_path Path to the file
         * @return Modified time as Unix timestamp, or 0 on error
         */
        static int64_t getFileModifiedTime(const std::string &file_path);

        /**
         * @brief Get current Unix timestamp
         *
         * @return Current time as Unix timestamp in seconds
         */
        static int64_t getCurrentTimestamp();
    };

} // namespace MediaDedup
