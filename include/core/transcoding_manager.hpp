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
     * @brief Get singleton instance
     * @return Reference to the singleton instance
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

    /**
     * @brief Queue a file for transcoding
     * @param file_path Path to the raw file to transcode
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
     * @brief Shutdown the transcoding manager
     */
    void shutdown();

private:
    TranscodingManager() = default;
    ~TranscodingManager() = default;
    TranscodingManager(const TranscodingManager &) = delete;
    TranscodingManager &operator=(const TranscodingManager &) = delete;

    /**
     * @brief Transcoding thread function
     */
    void transcodingThreadFunction();

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

    // Member variables
    std::string cache_dir_;
    int max_threads_;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancelled_{false};

    // Cache size management
    std::atomic<size_t> max_cache_size_{10ULL * 1024 * 1024 * 1024}; // 10 GB default
    mutable std::mutex cache_size_mutex_;

    // Thread management
    std::vector<std::thread> transcoding_threads_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::queue<std::string> transcoding_queue_;

    // Statistics
    std::atomic<size_t> queued_count_{0};
    std::atomic<size_t> completed_count_{0};
    std::atomic<size_t> failed_count_{0};

    // Database manager reference
    DatabaseManager *db_manager_{nullptr};

    // Raw file extensions
    static const std::vector<std::string> raw_extensions_;
};