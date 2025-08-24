#pragma once

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <memory>
#include "database/database_manager.hpp"
#include "logging/logger.hpp"
#include <filesystem>

/**
 * @brief Transcoding manager for handling raw camera files
 *
 * This class manages the transcoding of raw camera files to standard formats
 * that can be processed by the media processor. It uses independent threads
 * to avoid blocking the main scanning and processing threads.
 */
class TranscodingManager
{
public:
    /**
     * @brief Cache cleanup configuration
     */
    struct CleanupConfig
    {
        int fully_processed_age_days = 7;     // Remove fully processed files older than 7 days
        int partially_processed_age_days = 3; // Remove partially processed files older than 3 days
        int unprocessed_age_days = 1;         // Remove unprocessed files older than 1 day
        bool require_all_modes = true;        // Require all modes to be processed for "fully processed"
        int cleanup_threshold_percent = 80;   // Start cleanup when 80% full
    };

    /**
     * @brief Cache entry with processing status and metadata
     */
    struct CacheEntry
    {
        std::string source_file;
        std::string cache_file;
        bool is_processed;             // Processed in at least one mode
        bool is_fully_processed;       // Processed in all enabled modes
        std::time_t cache_age;         // How old is the transcoded file
        size_t file_size;              // Cache file size
        std::string processing_status; // Human-readable processing status
    };

    /**
     * @brief Get singleton instance
     * @return Reference to singleton instance
     */
    static TranscodingManager &getInstance();

    /**
     * @brief Initialize the transcoding manager
     * @param cache_dir Directory where transcoded files will be stored
     * @param max_threads Maximum number of transcoding threads
     */
    void initialize(const std::string &cache_dir, int max_threads = 4);

    /**
     * @brief Start transcoding threads
     */
    void startTranscoding();

    /**
     * @brief Stop transcoding threads
     */
    void stopTranscoding();

    /**
     * @brief Check if a file is a raw camera file that needs transcoding
     * @param file_path Path to the file to check
     * @return true if the file needs transcoding
     */
    static bool isRawFile(const std::string &file_path);

    // Helper methods for safe database access
    bool isDatabaseAvailable() const;
    DatabaseManager *getDatabaseManager() const;

    // Check if fully initialized
    bool isInitialized() const { return initialized_; }

    /**
     * @brief Queue a file for transcoding
     * @param file_path Path to the raw file to transcode
     * @note This method prevents duplicate entries - if a file is already queued or transcoded, it will not be added again
     */
    void queueForTranscoding(const std::string &file_path);

    /**
     * @brief Get the transcoded file path for a source file
     * @param source_file_path Path to the source file
     * @return Path to the transcoded file, or empty if not transcoded
     */
    std::string getTranscodedFilePath(const std::string &source_file_path);

    /**
     * @brief Check if transcoding is running
     * @return true if transcoding threads are active
     */
    bool isTranscodingRunning() const;

    /**
     * @brief Get transcoding statistics
     * @return Pair of (queued_count, completed_count)
     */
    std::pair<size_t, size_t> getTranscodingStats() const;

    /**
     * @brief Get cache directory size in bytes
     * @return Total size of cache directory
     */
    size_t getCacheSize() const;

    /**
     * @brief Get cache directory size in human-readable format
     * @return Cache size as string (e.g., "1.5 GB")
     */
    std::string getCacheSizeString() const;

    /**
     * @brief Set maximum cache size in bytes
     * @param max_size_bytes Maximum cache size in bytes
     */
    void setMaxCacheSize(size_t max_size_bytes);

    /**
     * @brief Get maximum cache size in bytes
     * @return Maximum cache size in bytes
     */
    size_t getMaxCacheSize() const;

    /**
     * @brief Set cache cleanup configuration
     * @param fully_processed_days Days to keep fully processed files
     * @param partially_processed_days Days to keep partially processed files
     * @param unprocessed_days Days to keep unprocessed files
     * @param require_all_modes Whether to require all modes for "fully processed"
     * @param cleanup_threshold_percent Percentage threshold to start cleanup
     */
    void setCleanupConfig(int fully_processed_days, int partially_processed_days,
                          int unprocessed_days, bool require_all_modes = true,
                          int cleanup_threshold_percent = 80);

    /**
     * @brief Get current cleanup configuration
     * @return CleanupConfig structure with current settings
     */
    const CleanupConfig &getCleanupConfig() const;

    /**
     * @brief Restore transcoding queue from database on startup
     * @note This method should be called after database initialization to restore pending transcoding jobs
     */
    void restoreQueueFromDatabase();

    /**
     * @brief Reset all transcoding job statuses from 1 (in progress) to 0 (queued) on startup
     * This ensures a clean state when the server restarts
     */
    void resetTranscodingJobStatusesOnStartup();

    /**
     * @brief Check if cache is over size limit
     * @return true if cache size exceeds limit
     */
    bool isCacheOverLimit() const;

    /**
     * @brief Clean up cache directory to stay under size limit
     * @param force_cleanup If true, clean up even if under limit
     * @return Number of files removed
     */
    size_t cleanupCache(bool force_cleanup = false);

    /**
     * @brief Enhanced cache cleanup that considers source file changes
     * @param force_cleanup If true, clean up even if under limit
     * @return Number of files removed
     */
    size_t cleanupCacheEnhanced(bool force_cleanup = false);

    /**
     * @brief Smart cache cleanup that considers processing status and age
     * @param force_cleanup If true, clean up even if under limit
     * @return Number of files removed
     */
    size_t cleanupCacheSmart(bool force_cleanup = false);

    /**
     * @brief Shutdown the transcoding manager
     */
    void shutdown();

    /**
     * @brief Force sync in-memory queue with database (for debugging/testing)
     * @return Number of files synced
     */
    size_t forceSyncQueueWithDatabase();

    /**
     * @brief Retry transcoding files that are in transcoding error state (3)
     * @return Number of files retried
     */
    size_t retryTranscodingErrorFiles();

    /**
     * @brief Get cache entries with processing status from database
     * @return Vector of CacheEntry objects
     */
    std::vector<CacheEntry> getCacheEntriesWithStatus();

    /**
     * @brief Get next transcoding job from database (database-only approach)
     * @return File path of next job, or empty string if none available
     */
    std::string getNextTranscodingJob();

    /**
     * @brief Mark transcoding job as in progress (database-only approach)
     * @param file_path Path to the file
     * @return True if successfully marked
     */
    bool markJobInProgress(const std::string &file_path);

    /**
     * @brief Mark transcoding job as completed (database-only approach)
     * @param file_path Path to the file
     * @param output_path Path to transcoded output
     * @return True if successfully marked
     */
    bool markJobCompleted(const std::string &file_path, const std::string &output_path);

    /**
     * @brief Mark transcoding job as failed (database-only approach)
     * @param file_path Path to the file
     * @return True if successfully marked
     */
    bool markJobFailed(const std::string &file_path);

    /**
     * @brief Remove invalid cache files (source changed/missing)
     * @param entries Cache entries to analyze
     * @return Number of files removed
     */
    size_t removeInvalidFiles(const std::vector<CacheEntry> &entries);

    /**
     * @brief Remove processed old cache files
     * @param entries Cache entries to analyze
     * @return Number of files removed
     */
    size_t removeProcessedOldFiles(const std::vector<CacheEntry> &entries);

    /**
     * @brief Remove unprocessed old cache files
     * @param entries Cache entries to analyze
     * @return Number of files removed
     */
    size_t removeUnprocessedOldFiles(const std::vector<CacheEntry> &entries);

    /**
     * @brief Remove oldest valid files if still over limit
     * @param entries Cache entries to analyze
     * @return Number of files removed
     */
    size_t removeOldestValidFiles(const std::vector<CacheEntry> &entries);

    /**
     * @brief Check if cache entry is old enough for cleanup based on processing status
     * @param entry Cache entry to check
     * @return true if entry should be considered for cleanup
     */
    bool isOldEnoughForCleanup(const CacheEntry &entry) const;

    /**
     * @brief Remove a single cache entry (file + database record)
     * @param entry Cache entry to remove
     * @return true if removal was successful
     */
    bool removeCacheEntry(const CacheEntry &entry);

    /**
     * @brief Transcode a raw file using LibRaw directly
     * @param source_file_path Path to the source raw file
     * @param output_path Path for the output JPEG file
     * @return true if transcoding succeeded
     */
    bool transcodeRawFileDirectly(const std::string &source_file_path, const std::string &output_path);

    // Member variables
    std::atomic<bool> running_{false};
    std::atomic<size_t> processed_count_{0};
    std::atomic<size_t> failed_count_{0};
    std::atomic<size_t> skipped_count_{0};
    std::atomic<size_t> retry_count_{0};

    // Database manager for transcoding operations
    DatabaseManager *db_manager_{nullptr};

    // Cache management
    std::string cache_dir_;
    size_t max_cache_size_mb_{1024}; // 1GB default
    std::atomic<size_t> current_cache_size_mb_{0};

    // Cache cleanup configuration
    size_t cleanup_threshold_mb_{800}; // 800MB threshold for cleanup
    size_t cleanup_target_mb_{600};    // Target size after cleanup

    // LibRaw is not thread-safe, so we need a mutex for LibRaw operations
    mutable std::mutex libraw_mutex_;

    // Raw file extensions - now configuration-driven
    std::vector<std::string> raw_extensions_;

    // Threading and queue management
    int max_threads_{4};
    std::vector<std::thread> transcoding_threads_;
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> initialized_{false};
    std::condition_variable queue_cv_;
    std::atomic<size_t> queued_count_{0};
    std::atomic<size_t> completed_count_{0};

    // Cache size management
    std::atomic<size_t> max_cache_size_{1073741824}; // 1GB in bytes
    mutable std::mutex cache_size_mutex_;

    // Cleanup configuration
    CleanupConfig cleanup_config_;

    /**
     * @brief Get the transcoded file path for a source file
     * @param source_file_path Path to the source file
     * @return Path to the transcoded file, or empty string if not found
     */
    std::string getTranscodedFilePath(const std::string &source_file_path) const;

    // Database schema upgrade
    bool upgradeCacheMapSchema();

    /**
     * @brief Load configuration from server config manager
     */
    void loadConfiguration();

    /**
     * @brief Set the database manager instance
     * @param db_manager Pointer to the database manager
     */
    void setDatabaseManager(DatabaseManager *db_manager);

private:
    TranscodingManager() = default;
    ~TranscodingManager() = default;
    TranscodingManager(const TranscodingManager &) = delete;
    TranscodingManager &operator=(const TranscodingManager &) = delete;

    /**
     * @brief Transcoding thread function
     */
    void transcodingThread();

    /**
     * @brief Transcode a single raw file
     * @param source_file_path Path to the source raw file
     * @return Path to the transcoded file, or empty if failed
     */
    std::string transcodeFile(const std::string &source_file_path);

    /**
     * @brief Generate a unique cache filename
     * @param source_file_path Path to the source file
     * @return Unique cache filename
     */
    std::string generateCacheFilename(const std::string &source_file_path);
};