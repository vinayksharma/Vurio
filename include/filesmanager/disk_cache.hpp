#pragma once

#include <string>
#include <memory>
#include <mutex>
#include <filesystem>
#include <vector>
#include <istream>
#include <set>
#include "config/unified_observable_config.hpp"

namespace MediaDedup
{
    /**
     * @brief Manages a disk-based cache with automatic size management
     *
     * DiskCache provides file caching with configurable size limits and automatic
     * eviction of oldest files when space is needed. The cache persists across
     * server restarts and supports thread-safe concurrent operations.
     *
     * Features:
     * - FIFO eviction policy (oldest files removed first)
     * - Observable configuration (reacts to runtime config changes)
     * - Thread-safe operations
     * - Handles files larger than cache limit (with warning)
     * - Persistent across restarts
     */
    class DiskCache
    {
    public:
        /**
         * @brief Construct a new DiskCache object
         * @param config_manager Configuration manager for observable properties
         */
        explicit DiskCache(std::shared_ptr<UnifiedObservableConfigManager> config_manager);

        /**
         * @brief Destroy the DiskCache object
         */
        ~DiskCache();

        /**
         * @brief Initialize the cache (create directory, calculate size, subscribe to config)
         * @return true if initialization successful, false otherwise
         */
        bool initialize();

        /**
         * @brief Shutdown the cache (cleanup subscriptions)
         */
        void shutdown();

        /**
         * @brief Copy a file to the cache
         * @param source_path Path to the source file
         * @param cached_path Output parameter - path to the cached file
         * @return true if copy successful, false otherwise
         */
        bool copyToCache(const std::string &source_path, std::string &cached_path);

        /**
         * @brief Mark a file as in use (prevents deletion)
         * @param cached_path Path to the cached file
         */
        void markFileInUse(const std::string &cached_path);

        /**
         * @brief Mark a file as no longer in use (allows deletion)
         * @param cached_path Path to the cached file
         */
        void markFileNotInUse(const std::string &cached_path);

        /**
         * @brief Delete a file from cache immediately after processing
         * @param cached_path Path to the cached file to delete
         * @return true if deletion successful, false otherwise
         */
        bool deleteFromCacheImmediately(const std::string &cached_path);

        /**
         * @brief Save a stream to a file in the cache
         * @param stream Input stream to read from
         * @param filename Original filename (used for cache filename generation)
         * @param cached_path Output parameter - path to the cached file
         * @return true if save successful, false otherwise
         */
        bool saveStreamToCache(std::istream &stream, const std::string &filename, std::string &cached_path);

        /**
         * @brief Delete a specific file from the cache
         * @param cached_path Path to the cached file to delete
         * @return true if deletion successful, false otherwise
         */
        bool deleteFromCache(const std::string &cached_path);

        /**
         * @brief Clear all files from the cache
         * @return true if clear successful, false otherwise
         */
        bool clearCache();

        /**
         * @brief Get the current cache size in megabytes
         * @return Current cache size in MB
         */
        size_t getCurrentSizeMB() const;

        /**
         * @brief Get the configured cache size limit in megabytes
         * @return Size limit in MB
         */
        size_t getSizeLimitMB() const;

        /**
         * @brief Get the cache directory location
         * @return Cache directory path
         */
        std::string getCacheLocation() const;

    private:
        /**
         * @brief Enforce size limit by removing old files if necessary
         * @param required_space_bytes Space needed for new file
         */
        void enforceSizeLimit(size_t required_space_bytes);

        /**
         * @brief Remove oldest files to free up specified space
         * @param bytes_to_free Amount of space to free in bytes
         */
        void removeOldestFiles(size_t bytes_to_free);

        /**
         * @brief Calculate total size of all files in cache
         * @return Total size in bytes
         */
        size_t calculateCacheSize();

        /**
         * @brief Get list of files sorted by creation time (oldest first)
         * @return Vector of pairs (path, creation_time)
         */
        std::vector<std::pair<std::filesystem::path, std::filesystem::file_time_type>> getFilesByAge();

        /**
         * @brief Generate cache filename using original filename (no hash)
         * @param original_path Original file path
         * @return Cache filename (same as original filename)
         */
        std::string generateCacheFilename(const std::string &original_path);

        /**
         * @brief Configuration change handler for cache location
         * @param new_location New cache directory location
         */
        void onCacheLocationChanged(const std::string &new_location);

        /**
         * @brief Configuration change handler for size limit
         * @param new_limit_mb New size limit in megabytes
         */
        void onSizeLimitChanged(int new_limit_mb);

        std::shared_ptr<UnifiedObservableConfigManager> config_manager_;
        mutable std::mutex cache_mutex_;
        std::filesystem::path cache_location_;
        size_t size_limit_bytes_;
        size_t current_size_bytes_;
        bool initialized_;

        // File-in-use tracking
        std::set<std::string> files_in_use_;

        // Configuration property keys
        static constexpr const char *CONFIG_CACHE_LOCATION = "cache.disk.location";
        static constexpr const char *CONFIG_CACHE_SIZE_LIMIT_MB = "cache.disk.size_limit_mb";

        // Default values
        static constexpr const char *DEFAULT_CACHE_LOCATION = "cache";
        static constexpr int DEFAULT_CACHE_SIZE_LIMIT_MB = 1024; // 1GB
    };

} // namespace MediaDedup
