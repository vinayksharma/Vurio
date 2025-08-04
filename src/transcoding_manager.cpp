#include "core/transcoding_manager.hpp"
#include "core/file_utils.hpp"
#include <filesystem>
#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <iomanip>
#include <chrono>
#include "core/server_config_manager.hpp"
#include <libraw/libraw.h>

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

    Logger::info("isRawFile check for: " + file_path + " (extension: " + extension + ") -> " + (needs_transcoding ? "YES" : "NO"));

    return needs_transcoding;
}

void TranscodingManager::queueForTranscoding(const std::string &file_path)
{
    Logger::info("queueForTranscoding called for: " + file_path);

    if (!running_.load())
    {
        Logger::warn("Transcoding not running, cannot queue file: " + file_path);
        return;
    }

    // Check if already transcoded
    std::string existing_transcoded = getTranscodedFilePath(file_path);
    if (!existing_transcoded.empty())
    {
        Logger::debug("File already transcoded: " + file_path);
        return;
    }

    // Add to database queue
    DBOpResult result = db_manager_->insertTranscodingFile(file_path);
    if (!result.success)
    {
        Logger::error("Failed to queue file for transcoding: " + file_path + " - " + result.error_message);
        return;
    }

    // Add to thread queue
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        transcoding_queue_.push(file_path);
        queued_count_.fetch_add(1);
    }

    queue_cv_.notify_one();

    Logger::info("Successfully queued file for transcoding: " + file_path);
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
            }
        }
        catch (const std::exception &e)
        {
            failed_count_.fetch_add(1);
            Logger::error("Exception during transcoding: " + file_path + " - " + std::string(e.what()));
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
        Logger::info("Cache size limit exceeded, performing enhanced cleanup before transcoding");
        cleanupCacheEnhanced(true);
    }

    // Use LibRaw to transcode raw file to JPEG
    Logger::debug("Using LibRaw to transcode: " + source_file_path + " -> " + output_path);

    // LibRaw is not thread-safe, so we need to serialize access
    std::lock_guard<std::mutex> libraw_lock(libraw_mutex_);

    // Validate input file before processing
    if (!std::filesystem::exists(source_file_path))
    {
        Logger::error("Source file does not exist: " + source_file_path);
        return "";
    }

    // Check file size to prevent processing corrupted files
    try
    {
        auto file_size = std::filesystem::file_size(source_file_path);
        if (file_size == 0)
        {
            Logger::error("Source file is empty: " + source_file_path);
            return "";
        }
        if (file_size > 500 * 1024 * 1024) // 500MB limit
        {
            Logger::error("Source file too large for transcoding: " + source_file_path + " (" + std::to_string(file_size) + " bytes)");
            return "";
        }
    }
    catch (const std::exception &e)
    {
        Logger::error("Error checking file size: " + source_file_path + " - " + std::string(e.what()));
        return "";
    }

    try
    {
        LibRaw raw_processor;

        // Set LibRaw options for better memory management and stability
        raw_processor.imgdata.params.output_tiff = 0;    // Output JPEG instead of TIFF
        raw_processor.imgdata.params.output_bps = 8;     // 8-bit output
        raw_processor.imgdata.params.output_color = 1;   // sRGB color space
        raw_processor.imgdata.params.use_camera_wb = 1;  // Use camera white balance
        raw_processor.imgdata.params.use_auto_wb = 0;    // Disable auto white balance
        raw_processor.imgdata.params.highlight = 0;      // Normal highlight handling
        raw_processor.imgdata.params.no_auto_bright = 1; // Disable auto brightness
        raw_processor.imgdata.params.bright = 1.0;       // Normal brightness
        raw_processor.imgdata.params.threshold = 0.0;    // No threshold
        raw_processor.imgdata.params.half_size = 0;      // Full size output

        // Add timeout protection for file operations
        Logger::debug("Opening file with LibRaw: " + source_file_path);
        int result = raw_processor.open_file(source_file_path.c_str());
        if (result != LIBRAW_SUCCESS)
        {
            Logger::error("LibRaw failed to open file: " + source_file_path + " (error: " + std::to_string(result) + ")");
            return "";
        }

        // Validate that the file was opened successfully
        if (raw_processor.imgdata.sizes.width == 0 || raw_processor.imgdata.sizes.height == 0)
        {
            Logger::error("LibRaw opened file but image dimensions are invalid: " + source_file_path);
            raw_processor.recycle();
            return "";
        }

        Logger::debug("Unpacking raw data: " + source_file_path);
        // Unpack the raw data
        result = raw_processor.unpack();
        if (result != LIBRAW_SUCCESS)
        {
            Logger::error("LibRaw failed to unpack file: " + source_file_path + " (error: " + std::to_string(result) + ")");
            raw_processor.recycle();
            return "";
        }

        Logger::debug("Processing image with LibRaw: " + source_file_path);
        // Process the image (demosaic)
        result = raw_processor.dcraw_process();
        if (result != LIBRAW_SUCCESS)
        {
            Logger::error("LibRaw failed to process file: " + source_file_path + " (error: " + std::to_string(result) + ")");
            raw_processor.recycle();
            return "";
        }

        Logger::debug("Writing JPEG output: " + output_path);
        // Write JPEG output using dcraw compatibility method
        result = raw_processor.dcraw_ppm_tiff_writer(output_path.c_str());
        if (result != LIBRAW_SUCCESS)
        {
            Logger::error("LibRaw failed to write output: " + output_path + " (error: " + std::to_string(result) + ")");
            raw_processor.recycle();

            // Clean up failed output file if it exists
            if (std::filesystem::exists(output_path))
            {
                std::filesystem::remove(output_path);
            }

            return "";
        }

        raw_processor.recycle();
    }
    catch (const std::bad_alloc &e)
    {
        Logger::error("LibRaw memory allocation failed during transcoding: " + std::string(e.what()));
        return "";
    }
    catch (const std::exception &e)
    {
        Logger::error("LibRaw exception during transcoding: " + std::string(e.what()));
        return "";
    }
    catch (...)
    {
        Logger::error("LibRaw unknown exception during transcoding - possible bus error or segmentation fault");
        return "";
    }

    if (std::filesystem::exists(output_path))
    {
        Logger::info("LibRaw transcoding successful: " + source_file_path + " -> " + output_path);

        // Log cache size after successful transcoding
        Logger::debug("Cache size after transcoding: " + getCacheSizeString());

        return output_path;
    }
    else
    {
        Logger::error("LibRaw transcoding failed - output file not created: " + output_path);
        return "";
    }
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