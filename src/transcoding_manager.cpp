#include "core/transcoding_manager.hpp"
#include "core/media_processor.hpp"
#include "core/file_utils.hpp"
#include <filesystem>
#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <iomanip>
#include <chrono>
#include "core/server_config_manager.hpp"
#include <libraw/libraw.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <algorithm> // Required for std::clamp and std::max
#include <cstring>   // Required for std::memcpy

// Raw file extensions that need transcoding - now configuration-driven
// These are no longer used as we use ServerConfigManager::needsTranscoding()

TranscodingManager &TranscodingManager::getInstance()
{
    static TranscodingManager instance;
    return instance;
}

void TranscodingManager::initialize(const std::string &cache_dir, int max_threads)
{
    cache_dir_ = cache_dir;
    max_threads_ = max_threads;
    db_manager_ = &DatabaseManager::getInstance();

    // Create cache directory if it doesn't exist
    std::filesystem::create_directories(cache_dir_);

    Logger::info("TranscodingManager initialized with cache dir: " + cache_dir_ +
                 ", max threads: " + std::to_string(max_threads_));
}

void TranscodingManager::startTranscoding()
{
    if (running_.load())
    {
        Logger::warn("Transcoding is already running");
        return;
    }

    running_.store(true);
    cancelled_.store(false);

    // Start transcoding threads
    for (int i = 0; i < max_threads_; ++i)
    {
        transcoding_threads_.emplace_back(&TranscodingManager::transcodingThreadFunction, this);
    }

    Logger::info("Started " + std::to_string(max_threads_) + " transcoding threads");
}

void TranscodingManager::stopTranscoding()
{
    if (!running_.load())
    {
        return;
    }

    Logger::info("Stopping transcoding threads");

    cancelled_.store(true);
    queue_cv_.notify_all();

    // Wait for all threads to complete
    for (auto &thread : transcoding_threads_)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }

    transcoding_threads_.clear();
    running_.store(false);

    Logger::info("Transcoding threads stopped");
}

bool TranscodingManager::isRawFile(const std::string &file_path)
{
    std::string extension = MediaProcessor::getFileExtension(file_path);
    std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);

    // Use ServerConfigManager to check if this extension needs transcoding
    bool needs_transcoding = ServerConfigManager::getInstance().needsTranscoding(extension);

    return needs_transcoding;
}

void TranscodingManager::queueForTranscoding(const std::string &file_path)
{
    Logger::debug("queueForTranscoding called for: " + file_path);

    if (!running_.load())
    {
        Logger::warn("Transcoding not running, cannot queue file: " + file_path);
        return;
    }

    // Use a single mutex lock to keep database and in-memory queue operations atomic
    // This prevents race conditions where multiple threads could queue the same file
    std::lock_guard<std::mutex> lock(queue_mutex_);

    // Check if already transcoded (database check)
    std::string existing_transcoded = getTranscodedFilePath(file_path);
    if (!existing_transcoded.empty())
    {
        Logger::debug("File already transcoded: " + file_path);
        return;
    }

    // Check if already in the in-memory queue to prevent duplicates
    bool already_queued = false;
    std::queue<std::string> temp_queue = transcoding_queue_; // Create a copy to check
    while (!temp_queue.empty())
    {
        if (temp_queue.front() == file_path)
        {
            already_queued = true;
            break;
        }
        temp_queue.pop();
    }

    if (already_queued)
    {
        Logger::debug("File already queued for transcoding: " + file_path);
        return;
    }

    // Add to database queue first
    DBOpResult result = db_manager_->insertTranscodingFile(file_path);
    if (!result.success)
    {
        Logger::error("Failed to queue file for transcoding: " + file_path + " - " + result.error_message);
        return;
    }

    // Add to in-memory queue (still under the same mutex lock)
    transcoding_queue_.push(file_path);
    queued_count_.fetch_add(1);

    // Release the mutex before notifying (to avoid potential deadlock)
    // The mutex is automatically released when lock goes out of scope

    Logger::info("Successfully queued file for transcoding: " + file_path);

    // Notify transcoding threads that new work is available
    queue_cv_.notify_one();
}

std::string TranscodingManager::getTranscodedFilePath(const std::string &source_file_path)
{
    return db_manager_->getTranscodedFilePath(source_file_path);
}

bool TranscodingManager::isTranscodingRunning() const
{
    return running_.load();
}

std::pair<size_t, size_t> TranscodingManager::getTranscodingStats() const
{
    return {queued_count_.load(), completed_count_.load()};
}

void TranscodingManager::shutdown()
{
    stopTranscoding();
    Logger::info("TranscodingManager shutdown complete");
}

void TranscodingManager::transcodingThreadFunction()
{
    Logger::debug("Transcoding thread started");

    while (running_.load() && !cancelled_.load())
    {
        std::string file_path;

        // Get next file from queue
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this]
                           { return !transcoding_queue_.empty() || !running_.load() || cancelled_.load(); });

            if (!running_.load() || cancelled_.load())
            {
                break;
            }

            if (transcoding_queue_.empty())
            {
                continue;
            }

            file_path = transcoding_queue_.front();
            transcoding_queue_.pop();
        }

        Logger::info("Starting transcoding: " + file_path);

        try
        {
            Logger::info("Calling transcodeFile for: " + file_path);
            std::string transcoded_path = transcodeFile(file_path);
            Logger::info("transcodeFile returned: " + (transcoded_path.empty() ? "EMPTY" : transcoded_path));

            if (!transcoded_path.empty())
            {
                // Update database with transcoded file path
                Logger::info("Updating database with transcoded path: " + file_path + " -> " + transcoded_path);
                DBOpResult result = db_manager_->updateTranscodedFilePath(file_path, transcoded_path);
                if (result.success)
                {
                    completed_count_.fetch_add(1);
                    Logger::info("Transcoding completed successfully: " + file_path + " -> " + transcoded_path);

                    // CRITICAL: Reset processing flags from -1 (in progress) to 0 (retry)
                    // This allows the processing thread to pick up the file again
                    Logger::info("Resetting processing flags for transcoded file: " + file_path);
                    auto &config_manager = ServerConfigManager::getInstance();

                    // Get the current dedup mode and check if quality stack processing is enabled
                    DedupMode current_mode = config_manager.getDedupMode();
                    bool pre_process_quality_stack = config_manager.getPreProcessQualityStack();

                    // Determine which modes to reset based on configuration
                    std::vector<DedupMode> modes_to_reset;
                    if (pre_process_quality_stack)
                    {
                        // Reset all three quality levels
                        modes_to_reset = {DedupMode::FAST, DedupMode::BALANCED, DedupMode::QUALITY};
                    }
                    else
                    {
                        // Reset only the current mode
                        modes_to_reset = {current_mode};
                    }

                    for (const auto &mode : modes_to_reset)
                    {
                        auto flag_result = db_manager_->resetProcessingFlag(file_path, mode);
                        if (!flag_result.success)
                        {
                            Logger::warn("Failed to reset processing flag after transcoding for: " + file_path +
                                         " mode: " + DedupModes::getModeName(mode) + " - " + flag_result.error_message);
                        }
                        else
                        {
                            Logger::debug("Reset processing flag to retry (0) after transcoding for: " + file_path +
                                          " mode: " + DedupModes::getModeName(mode));
                        }
                    }
                }
                else
                {
                    failed_count_.fetch_add(1);
                    Logger::error("Failed to update transcoded file path in database: " + file_path + " - " + result.error_message);
                }
            }
            else
            {
                failed_count_.fetch_add(1);
                Logger::error("Transcoding failed - transcodeFile returned empty path: " + file_path);

                // CRITICAL: Reset processing flags from -1 (in progress) to 0 (retry) on transcoding failure
                // This allows the processing thread to retry the file in a future cycle
                Logger::info("Resetting processing flags for failed transcoding: " + file_path);
                auto &config_manager = ServerConfigManager::getInstance();

                // Get the current dedup mode and check if quality stack processing is enabled
                DedupMode current_mode = config_manager.getDedupMode();
                bool pre_process_quality_stack = config_manager.getPreProcessQualityStack();

                // Determine which modes to reset based on configuration
                std::vector<DedupMode> modes_to_reset;
                if (pre_process_quality_stack)
                {
                    // Reset all three quality levels
                    modes_to_reset = {DedupMode::FAST, DedupMode::BALANCED, DedupMode::QUALITY};
                }
                else
                {
                    // Reset only the current mode
                    modes_to_reset = {current_mode};
                }

                for (const auto &mode : modes_to_reset)
                {
                    auto flag_result = db_manager_->resetProcessingFlag(file_path, mode);
                    if (!flag_result.success)
                    {
                        Logger::warn("Failed to reset processing flag after transcoding failure for: " + file_path +
                                     " mode: " + DedupModes::getModeName(mode) + " - " + flag_result.error_message);
                    }
                    else
                    {
                        Logger::debug("Reset processing flag to retry (0) after transcoding failure for: " + file_path +
                                      " mode: " + DedupModes::getModeName(mode));
                    }
                }
            }
        }
        catch (const std::exception &e)
        {
            failed_count_.fetch_add(1);
            Logger::error("Exception during transcoding: " + file_path + " - " + std::string(e.what()));

            // CRITICAL: Reset processing flags from -1 (in progress) to 0 (retry) on transcoding exception
            // This allows the processing thread to retry the file in a future cycle
            Logger::info("Resetting processing flags for transcoding exception: " + file_path);
            try
            {
                auto &config_manager = ServerConfigManager::getInstance();

                // Get the current dedup mode and check if quality stack processing is enabled
                DedupMode current_mode = config_manager.getDedupMode();
                bool pre_process_quality_stack = config_manager.getPreProcessQualityStack();

                // Determine which modes to reset based on configuration
                std::vector<DedupMode> modes_to_reset;
                if (pre_process_quality_stack)
                {
                    // Reset all three quality levels
                    modes_to_reset = {DedupMode::FAST, DedupMode::BALANCED, DedupMode::QUALITY};
                }
                else
                {
                    // Reset only the current mode
                    modes_to_reset = {current_mode};
                }

                for (const auto &mode : modes_to_reset)
                {
                    auto flag_result = db_manager_->resetProcessingFlag(file_path, mode);
                    if (!flag_result.success)
                    {
                        Logger::warn("Failed to reset processing flag after transcoding exception for: " + file_path +
                                     " mode: " + DedupModes::getModeName(mode) + " - " + flag_result.error_message);
                    }
                    else
                    {
                        Logger::debug("Reset processing flag to retry (0) after transcoding exception for: " + file_path +
                                      " mode: " + DedupModes::getModeName(mode));
                    }
                }
            }
            catch (...)
            {
                Logger::error("Failed to reset processing flags after transcoding exception for: " + file_path);
            }
        }
    }

    Logger::debug("Transcoding thread stopped");
}

std::string TranscodingManager::transcodeFile(const std::string &source_file_path)
{
    std::string cache_filename = generateCacheFilename(source_file_path);
    std::string output_path = std::filesystem::path(cache_dir_) / cache_filename;

    // Check if already exists and is valid
    if (std::filesystem::exists(output_path))
    {
        // Use existing file change detection to check if cache is still valid
        auto current_metadata = FileUtils::getFileMetadata(source_file_path);
        if (current_metadata)
        {
            // Check if source file has changed using existing metadata system
            // This would integrate with the existing DatabaseManager metadata comparison
            Logger::debug("Transcoded file already exists: " + output_path);
            return output_path;
        }
        else
        {
            // Source file doesn't exist, remove invalid cache entry
            Logger::debug("Source file no longer exists, removing invalid cache: " + output_path);
            std::filesystem::remove(output_path);
        }
    }

    // Check cache size and cleanup if needed
    if (isCacheOverLimit())
    {
        Logger::info("Cache size limit exceeded, performing smart cleanup before transcoding");
        cleanupCacheSmart(true);
    }

    // Use LibRaw directly for transcoding (no external executables)
    if (transcodeRawFileDirectly(source_file_path, output_path))
    {
        Logger::info("LibRaw transcoding succeeded: " + source_file_path + " -> " + output_path);
        return output_path;
    }
    Logger::error("LibRaw transcoding failed for: " + source_file_path);
    return "";
}

std::string TranscodingManager::generateCacheFilename(const std::string &source_file_path)
{
    // Generate a unique filename based on source file path hash
    std::string hash = MediaProcessor::generateHash(std::vector<uint8_t>(source_file_path.begin(), source_file_path.end()));

    // Get original extension and convert to jpg
    std::string original_ext = MediaProcessor::getFileExtension(source_file_path);
    std::transform(original_ext.begin(), original_ext.end(), original_ext.begin(), ::tolower);

    // Use first 16 characters of hash + original extension + .jpg
    return hash.substr(0, 16) + "_" + original_ext + ".jpg";
}

size_t TranscodingManager::getCacheSize() const
{
    std::lock_guard<std::mutex> lock(cache_size_mutex_);

    size_t total_size = 0;
    try
    {
        for (const auto &entry : std::filesystem::recursive_directory_iterator(cache_dir_))
        {
            if (entry.is_regular_file())
            {
                total_size += entry.file_size();
            }
        }
    }
    catch (const std::exception &e)
    {
        Logger::error("Error calculating cache size: " + std::string(e.what()));
    }

    return total_size;
}

std::string TranscodingManager::getCacheSizeString() const
{
    size_t size_bytes = getCacheSize();

    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit_index = 0;
    double size_double = static_cast<double>(size_bytes);

    while (size_double >= 1024.0 && unit_index < 4)
    {
        size_double /= 1024.0;
        unit_index++;
    }

    std::stringstream ss;
    ss << std::fixed << std::setprecision(1) << size_double << " " << units[unit_index];
    return ss.str();
}

void TranscodingManager::setMaxCacheSize(size_t max_size_bytes)
{
    max_cache_size_.store(max_size_bytes);
    Logger::info("Cache size limit set to: " + std::to_string(max_size_bytes) + " bytes");
}

size_t TranscodingManager::getMaxCacheSize() const
{
    return max_cache_size_.load();
}

void TranscodingManager::setCleanupConfig(int fully_processed_days, int partially_processed_days,
                                          int unprocessed_days, bool require_all_modes,
                                          int cleanup_threshold_percent)
{
    std::lock_guard<std::mutex> lock(cache_size_mutex_);

    cleanup_config_.fully_processed_age_days = std::max(1, fully_processed_days);
    cleanup_config_.partially_processed_age_days = std::max(1, partially_processed_days);
    cleanup_config_.unprocessed_age_days = std::max(1, unprocessed_days);
    cleanup_config_.require_all_modes = require_all_modes;
    cleanup_config_.cleanup_threshold_percent = std::clamp(cleanup_threshold_percent, 50, 95);

    Logger::info("Cache cleanup configuration updated: " +
                 std::string("Fully processed: ") + std::to_string(cleanup_config_.fully_processed_age_days) + " days, " +
                 std::string("Partially processed: ") + std::to_string(cleanup_config_.partially_processed_age_days) + " days, " +
                 std::string("Unprocessed: ") + std::to_string(cleanup_config_.unprocessed_age_days) + " days, " +
                 std::string("Require all modes: ") + (cleanup_config_.require_all_modes ? "true" : "false") + ", " +
                 std::string("Cleanup threshold: ") + std::to_string(cleanup_config_.cleanup_threshold_percent) + "%");
}

const TranscodingManager::CleanupConfig &TranscodingManager::getCleanupConfig() const
{
    return cleanup_config_;
}

bool TranscodingManager::isCacheOverLimit() const
{
    return getCacheSize() > getMaxCacheSize();
}

size_t TranscodingManager::cleanupCache(bool force_cleanup)
{
    std::lock_guard<std::mutex> lock(cache_size_mutex_);

    size_t current_size = getCacheSize();
    size_t max_size = getMaxCacheSize();

    if (!force_cleanup && current_size <= max_size)
    {
        Logger::debug("Cache size (" + getCacheSizeString() + ") is under limit, no cleanup needed");
        return 0;
    }

    Logger::info("Starting cache cleanup. Current size: " + getCacheSizeString() +
                 ", Max size: " + std::to_string(max_size / (1024 * 1024)) + " MB");

    // Get all cache files and sort by modification time (oldest first)
    std::vector<std::filesystem::path> cache_files;
    try
    {
        for (const auto &entry : std::filesystem::recursive_directory_iterator(cache_dir_))
        {
            if (entry.is_regular_file())
            {
                cache_files.push_back(entry.path());
            }
        }
    }
    catch (const std::exception &e)
    {
        Logger::error("Error scanning cache directory: " + std::string(e.what()));
        return 0;
    }

    // Sort by modification time (oldest first)
    std::sort(cache_files.begin(), cache_files.end(),
              [](const auto &a, const auto &b)
              {
                  return std::filesystem::last_write_time(a) < std::filesystem::last_write_time(b);
              });

    // Remove oldest files until under limit
    size_t files_removed = 0;
    size_t bytes_freed = 0;

    for (const auto &cache_file : cache_files)
    {
        if (current_size <= max_size)
        {
            break;
        }

        try
        {
            size_t file_size = std::filesystem::file_size(cache_file);
            std::filesystem::remove(cache_file);

            current_size -= file_size;
            bytes_freed += file_size;
            files_removed++;

            Logger::debug("Removed cache file: " + cache_file.string() +
                          " (size: " + std::to_string(file_size) + " bytes)");
        }
        catch (const std::exception &e)
        {
            Logger::error("Error removing cache file " + cache_file.string() + ": " + std::string(e.what()));
        }
    }

    Logger::info("Cache cleanup completed. Removed " + std::to_string(files_removed) +
                 " files, freed " + std::to_string(bytes_freed / (1024 * 1024)) + " MB");

    return files_removed;
}

size_t TranscodingManager::cleanupCacheEnhanced(bool force_cleanup)
{
    std::lock_guard<std::mutex> lock(cache_size_mutex_);

    size_t current_size = getCacheSize();
    size_t max_size = getMaxCacheSize();

    if (!force_cleanup && current_size <= max_size)
    {
        Logger::debug("Cache size (" + getCacheSizeString() + ") is under limit, no cleanup needed");
        return 0;
    }

    Logger::info("Starting enhanced cache cleanup. Current size: " + getCacheSizeString() +
                 ", Max size: " + std::to_string(max_size / (1024 * 1024)) + " MB");

    // Get all cache entries from database
    std::vector<std::pair<std::string, std::string>> cache_entries;
    try
    {
        // This would come from DatabaseManager::getAllCacheEntries()
        // For now, we'll scan the cache directory
        for (const auto &entry : std::filesystem::recursive_directory_iterator(cache_dir_))
        {
            if (entry.is_regular_file())
            {
                // Extract source file path from cache filename (reverse the hash)
                // This is simplified - in practice, we'd store the mapping in database
                std::string cache_file = entry.path().string();
                std::string source_file = "unknown"; // Would be retrieved from database

                cache_entries.emplace_back(source_file, cache_file);
            }
        }
    }
    catch (const std::exception &e)
    {
        Logger::error("Error scanning cache directory: " + std::string(e.what()));
        return 0;
    }

    // Use existing file change detection system
    std::vector<std::pair<std::string, std::string>> invalid_files; // Source file changed
    std::vector<std::pair<std::string, std::string>> valid_files;   // Source file unchanged

    for (const auto &[source_file, cache_file] : cache_entries)
    {
        // Use existing FileUtils::getFileMetadata and database comparison
        auto current_metadata = FileUtils::getFileMetadata(source_file);
        if (!current_metadata)
        {
            // Source file doesn't exist, mark as invalid
            invalid_files.emplace_back(source_file, cache_file);
            continue;
        }

        // Get stored metadata from database (this would be implemented in DatabaseManager)
        // For now, we'll assume files are valid if they exist
        if (std::filesystem::exists(cache_file))
        {
            valid_files.emplace_back(source_file, cache_file);
        }
        else
        {
            invalid_files.emplace_back(source_file, cache_file);
        }
    }

    Logger::info("Cache analysis: " + std::to_string(valid_files.size()) + " valid files, " +
                 std::to_string(invalid_files.size()) + " invalid files");

    // Remove invalid files first (source file changed or doesn't exist)
    size_t files_removed = 0;
    size_t bytes_freed = 0;

    for (const auto &[source_file, cache_file] : invalid_files)
    {
        try
        {
            size_t file_size = std::filesystem::file_size(cache_file);
            std::filesystem::remove(cache_file);

            current_size -= file_size;
            bytes_freed += file_size;
            files_removed++;

            Logger::debug("Removed invalid cache file: " + cache_file +
                          " (source changed or missing, size: " + std::to_string(file_size) + " bytes)");
        }
        catch (const std::exception &e)
        {
            Logger::error("Error removing invalid cache file " + cache_file + ": " + std::string(e.what()));
        }
    }

    // If still over limit, remove oldest valid files
    if (current_size > max_size)
    {
        Logger::info("Still over limit after removing invalid files, removing oldest valid files");

        // Sort valid files by modification time (oldest first)
        std::sort(valid_files.begin(), valid_files.end(),
                  [](const auto &a, const auto &b)
                  {
                      return std::filesystem::last_write_time(a.second) < std::filesystem::last_write_time(b.second);
                  });

        // Remove oldest files until under limit
        for (const auto &[source_file, cache_file] : valid_files)
        {
            if (current_size <= max_size)
            {
                break;
            }

            try
            {
                size_t file_size = std::filesystem::file_size(cache_file);
                std::filesystem::remove(cache_file);

                current_size -= file_size;
                bytes_freed += file_size;
                files_removed++;

                Logger::debug("Removed oldest valid cache file: " + cache_file +
                              " (size: " + std::to_string(file_size) + " bytes)");
            }
            catch (const std::exception &e)
            {
                Logger::error("Error removing oldest cache file " + cache_file + ": " + std::string(e.what()));
            }
        }
    }

    Logger::info("Enhanced cache cleanup completed. Removed " + std::to_string(files_removed) +
                 " files, freed " + std::to_string(bytes_freed / (1024 * 1024)) + " MB");

    return files_removed;
}

size_t TranscodingManager::cleanupCacheSmart(bool force_cleanup)
{
    std::lock_guard<std::mutex> lock(cache_size_mutex_);

    size_t current_size = getCacheSize();
    size_t max_size = getMaxCacheSize();

    if (!force_cleanup && current_size <= max_size)
    {
        Logger::debug("Cache size (" + getCacheSizeString() + ") is under limit, no cleanup needed");
        return 0;
    }

    Logger::info("Starting smart cache cleanup. Current size: " + getCacheSizeString() +
                 ", Max size: " + std::to_string(max_size / (1024 * 1024)) + " MB");

    // Get cache entries with processing status from database
    std::vector<CacheEntry> cache_entries = getCacheEntriesWithStatus();

    if (cache_entries.empty())
    {
        Logger::info("No cache entries found for smart cleanup");
        return 0;
    }

    Logger::info("Analyzing " + std::to_string(cache_entries.size()) + " cache entries for smart cleanup");

    // Phase 1: Remove invalid files (source changed/missing)
    size_t invalid_removed = removeInvalidFiles(cache_entries);
    Logger::info("Phase 1 completed: Removed " + std::to_string(invalid_removed) + " invalid files");

    // Phase 2: Remove processed old files
    size_t processed_removed = removeProcessedOldFiles(cache_entries);
    Logger::info("Phase 2 completed: Removed " + std::to_string(processed_removed) + " processed old files");

    // Phase 3: Remove unprocessed old files
    size_t unprocessed_removed = removeUnprocessedOldFiles(cache_entries);
    Logger::info("Phase 3 completed: Removed " + std::to_string(unprocessed_removed) + " unprocessed old files");

    // Phase 4: If still over limit, remove oldest valid files
    size_t oldest_removed = 0;
    if (getCacheSize() > max_size)
    {
        Logger::info("Still over limit after smart cleanup, removing oldest valid files");
        oldest_removed = removeOldestValidFiles(cache_entries);
        Logger::info("Phase 4 completed: Removed " + std::to_string(oldest_removed) + " oldest valid files");
    }

    size_t total_removed = invalid_removed + processed_removed + unprocessed_removed + oldest_removed;
    size_t final_size = getCacheSize();

    Logger::info("Smart cache cleanup completed. Total files removed: " + std::to_string(total_removed) +
                 ", Final cache size: " + getCacheSizeString() +
                 ", Space freed: " + std::to_string((current_size - final_size) / (1024 * 1024)) + " MB");

    return total_removed;
}

std::vector<TranscodingManager::CacheEntry> TranscodingManager::getCacheEntriesWithStatus()
{
    std::vector<CacheEntry> entries;

    try
    {
        if (!db_manager_)
        {
            Logger::error("Database manager not available for cache status lookup");
            return entries;
        }

        // This would be implemented in DatabaseManager to get cache entries with processing status
        // For now, we'll scan the cache directory and build entries manually
        for (const auto &entry : std::filesystem::recursive_directory_iterator(cache_dir_))
        {
            if (entry.is_regular_file())
            {
                std::string cache_file = entry.path().string();

                // Extract source file path from cache filename (reverse the hash)
                // This is simplified - in practice, we'd get this from database
                std::string source_file = "unknown"; // Would be retrieved from database

                // Get file metadata
                auto current_metadata = FileUtils::getFileMetadata(source_file);
                bool source_exists = current_metadata.has_value();

                // Get processing status from database (would be implemented)
                bool processed_fast = false;
                bool processed_balanced = false;
                bool processed_quality = false;

                // Determine processing status
                bool is_processed = processed_fast || processed_balanced || processed_quality;
                bool is_fully_processed = processed_fast && processed_balanced && processed_quality;

                // Calculate cache age
                std::time_t cache_age = 0;
                try
                {
                    auto cache_time = std::filesystem::last_write_time(entry.path());
                    auto now = std::chrono::system_clock::now();
                    auto cache_duration = cache_time.time_since_epoch();
                    auto now_duration = now.time_since_epoch();
                    auto age_duration = now_duration - cache_duration;
                    cache_age = std::chrono::duration_cast<std::chrono::seconds>(age_duration).count();
                }
                catch (...)
                {
                    cache_age = std::time(nullptr);
                }

                // Get file size
                size_t file_size = std::filesystem::file_size(entry.path());

                // Create processing status string
                std::string processing_status;
                if (!source_exists)
                {
                    processing_status = "SOURCE_MISSING";
                }
                else if (is_fully_processed)
                {
                    processing_status = "FULLY_PROCESSED";
                }
                else if (is_processed)
                {
                    processing_status = "PARTIALLY_PROCESSED";
                }
                else
                {
                    processing_status = "UNPROCESSED";
                }

                CacheEntry cache_entry{
                    source_file,
                    cache_file,
                    is_processed,
                    is_fully_processed,
                    cache_age,
                    file_size,
                    processing_status};

                entries.push_back(cache_entry);
            }
        }
    }
    catch (const std::exception &e)
    {
        Logger::error("Error getting cache entries with status: " + std::string(e.what()));
    }

    return entries;
}

size_t TranscodingManager::removeInvalidFiles(const std::vector<CacheEntry> &entries)
{
    size_t files_removed = 0;

    for (const auto &entry : entries)
    {
        if (entry.processing_status == "SOURCE_MISSING")
        {
            if (removeCacheEntry(entry))
            {
                files_removed++;
                Logger::debug("Removed invalid cache file (source missing): " + entry.cache_file);
            }
        }
    }

    return files_removed;
}

size_t TranscodingManager::removeProcessedOldFiles(const std::vector<CacheEntry> &entries)
{
    size_t files_removed = 0;

    for (const auto &entry : entries)
    {
        if (entry.is_fully_processed && isOldEnoughForCleanup(entry))
        {
            if (removeCacheEntry(entry))
            {
                files_removed++;
                Logger::debug("Removed processed old cache file: " + entry.cache_file +
                              " (age: " + std::to_string(entry.cache_age) + "s, status: " + entry.processing_status + ")");
            }
        }
    }

    return files_removed;
}

size_t TranscodingManager::removeUnprocessedOldFiles(const std::vector<CacheEntry> &entries)
{
    size_t files_removed = 0;

    for (const auto &entry : entries)
    {
        if (!entry.is_processed && isOldEnoughForCleanup(entry))
        {
            if (removeCacheEntry(entry))
            {
                files_removed++;
                Logger::debug("Removed unprocessed old cache file: " + entry.cache_file +
                              " (age: " + std::to_string(entry.cache_age) + "s, status: " + entry.processing_status + ")");
            }
        }
    }

    return files_removed;
}

size_t TranscodingManager::removeOldestValidFiles(const std::vector<CacheEntry> &entries)
{
    size_t files_removed = 0;
    size_t current_size = getCacheSize();
    size_t max_size = getMaxCacheSize();

    // Filter valid entries (source exists, not already removed)
    std::vector<CacheEntry> valid_entries;
    for (const auto &entry : entries)
    {
        if (entry.processing_status != "SOURCE_MISSING" &&
            std::filesystem::exists(entry.cache_file))
        {
            valid_entries.push_back(entry);
        }
    }

    // Sort by cache age (oldest first)
    std::sort(valid_entries.begin(), valid_entries.end(),
              [](const auto &a, const auto &b)
              {
                  return a.cache_age < b.cache_age;
              });

    // Remove oldest files until under limit
    for (const auto &entry : valid_entries)
    {
        if (current_size <= max_size)
        {
            break;
        }

        if (removeCacheEntry(entry))
        {
            current_size -= entry.file_size;
            files_removed++;
            Logger::debug("Removed oldest valid cache file: " + entry.cache_file +
                          " (age: " + std::to_string(entry.cache_age) + "s, size: " + std::to_string(entry.file_size) + " bytes)");
        }
    }

    return files_removed;
}

bool TranscodingManager::isOldEnoughForCleanup(const CacheEntry &entry) const
{
    auto now = std::time(nullptr);
    auto age_seconds = now - entry.cache_age;

    if (entry.is_fully_processed)
    {
        // Fully processed files: remove after configured age
        return age_seconds > (cleanup_config_.fully_processed_age_days * 24 * 3600);
    }
    else if (entry.is_processed)
    {
        // Partially processed files: remove after configured age
        return age_seconds > (cleanup_config_.partially_processed_age_days * 24 * 3600);
    }
    else
    {
        // Unprocessed files: remove after configured age
        return age_seconds > (cleanup_config_.unprocessed_age_days * 24 * 3600);
    }
}

bool TranscodingManager::removeCacheEntry(const CacheEntry &entry)
{
    try
    {
        // Remove cache file
        if (std::filesystem::exists(entry.cache_file))
        {
            std::filesystem::remove(entry.cache_file);
        }

        // Remove database record
        if (db_manager_)
        {
            DBOpResult result = db_manager_->removeTranscodingRecord(entry.source_file);
            if (!result.success)
            {
                Logger::warn("Failed to remove transcoding record from database: " + entry.source_file +
                             " - " + result.error_message);
            }
        }

        return true;
    }
    catch (const std::exception &e)
    {
        Logger::error("Error removing cache entry: " + entry.cache_file + " - " + std::string(e.what()));
        return false;
    }
}

void TranscodingManager::restoreQueueFromDatabase()
{
    Logger::info("Restoring transcoding queue from database...");

    // Check change flag to avoid redundant surveys
    try
    {
        if (db_manager_)
        {
            bool changed = db_manager_->getFlag("transcode_preprocess_scanned_files_changed");
            if (!changed)
            {
                Logger::info("Transcoding survey skipped: no changes in scanned_files (flag not set)");
                return;
            }
        }
    }
    catch (const std::exception &e)
    {
        Logger::warn(std::string("Error checking change flag; proceeding with survey: ") + e.what());
    }

    try
    {
        // Get all files that need transcoding from the database
        std::vector<std::string> files_needing_transcoding = db_manager_->getFilesNeedingTranscoding();

        if (files_needing_transcoding.empty())
        {
            Logger::info("No pending transcoding jobs found in database");
            return;
        }

        Logger::info("Found " + std::to_string(files_needing_transcoding.size()) + " pending transcoding jobs in database");

        // Fast restoration: add all files to queue without validation
        // Worker threads will handle file validation and cleanup during processing
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            for (const auto &file_path : files_needing_transcoding)
            {
                transcoding_queue_.push(file_path);
            }
        }

        Logger::info("Restored " + std::to_string(files_needing_transcoding.size()) + " transcoding jobs to queue");

        // Notify worker threads that new work is available
        queue_cv_.notify_all();
    }
    catch (const std::exception &e)
    {
        Logger::error("Error restoring transcoding queue from database: " + std::string(e.what()));
    }

    // Reset the change flag now that survey is complete
    try
    {
        if (db_manager_)
        {
            auto res = db_manager_->setFlag("transcode_preprocess_scanned_files_changed", false);
            if (!res.success)
            {
                Logger::warn("Failed to reset transcode_preprocess_scanned_files_changed flag: " + res.error_message);
            }
        }
    }
    catch (const std::exception &e)
    {
        Logger::warn(std::string("Exception resetting flag: ") + e.what());
    }
}

bool TranscodingManager::transcodeRawFileDirectly(const std::string &source_file_path, const std::string &output_path)
{
    std::lock_guard<std::mutex> lock(libraw_mutex_);

    LibRaw *raw = nullptr;
    libraw_processed_image_t *img = nullptr;

    try
    {
        // Validate input file exists and is readable
        if (!std::filesystem::exists(source_file_path))
        {
            Logger::error("Source file does not exist: " + source_file_path);
            return false;
        }

        // Ensure parent directory exists
        std::filesystem::create_directories(std::filesystem::path(output_path).parent_path());

        // Create LibRaw instance
        raw = new LibRaw();
        if (!raw)
        {
            Logger::error("Failed to create LibRaw instance for: " + source_file_path);
            return false;
        }

        // Configure LibRaw parameters
        raw->imgdata.params.use_camera_wb = 1;
        raw->imgdata.params.use_auto_wb = 0;
        raw->imgdata.params.no_auto_bright = 1;
        raw->imgdata.params.output_bps = 8;
        raw->imgdata.params.output_color = 1; // sRGB
        raw->imgdata.params.half_size = 0;
        raw->imgdata.params.output_tiff = 0; // JPEG output

        Logger::debug("Opening RAW file: " + source_file_path);
        int rc = raw->open_file(source_file_path.c_str());
        if (rc != LIBRAW_SUCCESS)
        {
            Logger::error("LibRaw open_file failed: " + std::string(libraw_strerror(rc)) + " (" + std::to_string(rc) + ") for: " + source_file_path);
            return false;
        }

        Logger::debug("Unpacking RAW data for: " + source_file_path);
        rc = raw->unpack();
        if (rc != LIBRAW_SUCCESS)
        {
            Logger::error("LibRaw unpack failed: " + std::string(libraw_strerror(rc)) + " (" + std::to_string(rc) + ") for: " + source_file_path);
            return false;
        }

        Logger::debug("Processing RAW data for: " + source_file_path);
        rc = raw->dcraw_process();
        if (rc != LIBRAW_SUCCESS)
        {
            Logger::error("LibRaw dcraw_process failed: " + std::string(libraw_strerror(rc)) + " (" + std::to_string(rc) + ") for: " + source_file_path);
            return false;
        }

        Logger::debug("Creating memory image for: " + source_file_path);
        img = raw->dcraw_make_mem_image(&rc);
        if (!img || rc != LIBRAW_SUCCESS)
        {
            Logger::error("LibRaw dcraw_make_mem_image failed: " + std::string(libraw_strerror(rc)) + " (" + std::to_string(rc) + ") for: " + source_file_path);
            return false;
        }

        // Validate image buffer
        if (img->type != LIBRAW_IMAGE_BITMAP)
        {
            Logger::error("Unsupported image type: " + std::to_string(img->type) + " for: " + source_file_path);
            return false;
        }

        if (img->colors != 3)
        {
            Logger::error("Unsupported color channels: " + std::to_string(img->colors) + " for: " + source_file_path);
            return false;
        }

        if (img->bits != 8)
        {
            Logger::error("Unsupported bit depth: " + std::to_string(img->bits) + " for: " + source_file_path);
            return false;
        }

        if (img->width <= 0 || img->height <= 0)
        {
            Logger::error("Invalid image dimensions: " + std::to_string(img->width) + "x" + std::to_string(img->height) + " for: " + source_file_path);
            return false;
        }

        // Note: img->data is an array, so it's always valid if img exists

        Logger::debug("Creating OpenCV Mat for: " + source_file_path + " (" + std::to_string(img->width) + "x" + std::to_string(img->height) + ")");

        // Create a copy of the data to avoid memory issues
        size_t data_size = img->width * img->height * 3;
        std::vector<unsigned char> rgb_data(data_size);
        std::memcpy(rgb_data.data(), img->data, data_size);

        // Construct cv::Mat with copied data (RGB to BGR for OpenCV)
        cv::Mat rgb(img->height, img->width, CV_8UC3, rgb_data.data());
        cv::Mat bgr;
        cv::cvtColor(rgb, bgr, cv::COLOR_RGB2BGR);

        Logger::debug("Writing JPEG output: " + output_path);
        std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, 92};
        if (!cv::imwrite(output_path, bgr, params))
        {
            Logger::error("OpenCV imwrite failed for: " + output_path);
            return false;
        }

        Logger::info("Successfully transcoded: " + source_file_path + " -> " + output_path);
        return true;
    }
    catch (const std::exception &e)
    {
        Logger::error("Exception during LibRaw transcoding: " + std::string(e.what()) + " for: " + source_file_path);
        return false;
    }
    catch (...)
    {
        Logger::error("Unknown exception during LibRaw transcoding for: " + source_file_path);
        return false;
    }

    // Cleanup section - always executed
    if (img)
    {
        try
        {
            LibRaw::dcraw_clear_mem(img);
        }
        catch (...)
        {
            Logger::warn("Exception during image memory cleanup for: " + source_file_path);
        }
    }

    if (raw)
    {
        try
        {
            raw->recycle();
            delete raw;
        }
        catch (...)
        {
            Logger::warn("Exception during LibRaw cleanup for: " + source_file_path);
        }
    }

    return false;
}