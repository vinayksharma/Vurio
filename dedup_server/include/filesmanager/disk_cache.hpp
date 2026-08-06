#pragma once

#include <string>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <filesystem>
#include <unordered_map>
#include <vector>
#include <istream>
#include <set>
#include "config/unified_observable_config.hpp"

namespace MediaDedup
{
    /**
     * @brief Manages a disk-based cache with automatic size management
     *
     * MP-2: accounting is an in-memory index (path -> size/insert-time) built
     * once at initialize() and maintained incrementally — no readdir/stat
     * storms per insert (previously ~180 ms per insert at 59k files, under
     * the mutex). Bulk file copies happen OUTSIDE the mutex. Eviction is
     * LRU-driven by the owning tables' last_accessed_at via injectable hooks
     * (setEvictionHooks), with the index's insert-order as fallback, batched
     * to 10% headroom so it doesn't re-trigger on every insert. Cache files
     * are sharded into 256 subdirectories and named by a hash of the FULL
     * source path (basename-only naming silently collided).
     *
     * Features:
     * - DB-driven LRU eviction (hooks), insert-order FIFO fallback
     * - Observable configuration (reacts to runtime config changes)
     * - Thread-safe operations; copies do not block other cache users
     * - Handles files larger than cache limit (refused, with warning)
     * - Persistent across restarts
     */
    class DiskCache
    {
    public:
        /**
         * @brief Construct a new DiskCache object
         * @param config_manager Configuration manager for observable properties
         * @param config_prefix Configuration key prefix (e.g., "cache.disk" or "cache.thumbnail")
         */
        explicit DiskCache(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                           const std::string &config_prefix);
        
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
         * @brief Move a file to the cache (more efficient than copy)
         * @param source_path Path to the source file
         * @param cached_path Output parameter - path to the cached file
         * @return true if move successful, false otherwise
         */
        bool moveToCache(const std::string &source_path, std::string &cached_path);

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

        /**
         * @brief MP-2: wire DB-driven LRU eviction.
         * @param lru_query returns cached-file paths in least-recently-used
         *        order (up to max_count) — typically SELECT cached_path ...
         *        ORDER BY last_accessed_at
         * @param evicted_notify called with the paths actually deleted so the
         *        owning table's rows can be removed (prevents dangling rows
         *        and the regeneration storms they cause)
         */
        using LruQueryFn = std::function<std::vector<std::string>(std::size_t max_count)>;
        using EvictedNotifyFn = std::function<void(const std::vector<std::string> &)>;
        void setEvictionHooks(LruQueryFn lru_query, EvictedNotifyFn evicted_notify);

    private:
        /// MP-2: per-file accounting entry (insert time approximates LRU for
        /// the no-hook fallback).
        struct IndexEntry
        {
            std::size_t size = 0;
            std::chrono::steady_clock::time_point inserted_at{};
        };

        /**
         * @brief Enforce size limit by removing old files if necessary.
         * Must be called with cache_mutex_ held.
         * @param required_space_bytes Space needed for new file
         */
        void enforceSizeLimitLocked(size_t required_space_bytes);

        /**
         * @brief Evict files to free at least bytes_to_free (LRU hook first,
         * index insert-order fallback). Must be called with cache_mutex_ held.
         */
        void removeOldestFilesLocked(size_t bytes_to_free);

        /// Rebuild the index from disk (recursive). cache_mutex_ held.
        void rebuildIndexLocked();

        void indexInsertLocked(const std::string &path, std::size_t size);
        void indexEraseLocked(const std::string &path);
        /// True when the path is marked in use or has a write in flight.
        bool isBusyLocked(const std::string &path) const;

        /**
         * @brief Sharded cache-relative name for a source path:
         * "<2-hex-shard>/<stem>_<16-hex-hash-of-full-path><ext>". Hashing the
         * FULL path (not the basename) makes distinct sources collision-free.
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
        // AR-6: RAII subscription — unsubscribes only itself on destruction.
        ScopedConfigSubscription config_subscription_;
        mutable std::mutex cache_mutex_;
        std::filesystem::path cache_location_;
        size_t size_limit_bytes_;
        size_t current_size_bytes_;
        bool initialized_;
        std::string config_prefix_;

        // MP-2: incremental accounting + in-flight write guard
        std::unordered_map<std::string, IndexEntry> index_;
        std::set<std::string> pending_writes_;
        LruQueryFn lru_query_;
        EvictedNotifyFn evicted_notify_;

        // File-in-use tracking
        std::set<std::string> files_in_use_;
        // Configuration property keys (constructed from prefix)
        std::string config_key_size_limit_;
        std::string config_key_clear_on_startup_;

        // Default values
        static constexpr const char *DEFAULT_CACHE_LOCATION = "cache";
        static constexpr int DEFAULT_CACHE_SIZE_LIMIT_MB = 1024; // 1GB
    };

} // namespace MediaDedup
