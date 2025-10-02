#include "filesmanager/disk_cache.hpp"
#include <Poco/Logger.h>
#include <Poco/LogStream.h>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <functional>

namespace MediaDedup
{
    DiskCache::DiskCache(std::shared_ptr<UnifiedObservableConfigManager> config_manager)
        : config_manager_(config_manager), cache_location_(DEFAULT_CACHE_LOCATION), size_limit_bytes_(DEFAULT_CACHE_SIZE_LIMIT_MB * 1024ULL * 1024ULL), current_size_bytes_(0), initialized_(false)
    {
    }

    DiskCache::~DiskCache()
    {
        shutdown();
    }

    bool DiskCache::initialize()
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);

        if (initialized_)
        {
            Poco::Logger::get("DiskCache").warning("DiskCache already initialized");
            return true;
        }

        try
        {
            // Get cache location from config
            std::string location = config_manager_->getPropertyValue<std::string>(
                CONFIG_CACHE_LOCATION, DEFAULT_CACHE_LOCATION);

            // Make it relative to working directory root
            cache_location_ = std::filesystem::current_path() / location;

            // Get size limit from config
            int size_limit_mb = config_manager_->getPropertyValue<int>(
                CONFIG_CACHE_SIZE_LIMIT_MB, DEFAULT_CACHE_SIZE_LIMIT_MB);
            size_limit_bytes_ = size_limit_mb * 1024ULL * 1024ULL;

            // Create cache directory if it doesn't exist
            if (!std::filesystem::exists(cache_location_))
            {
                std::filesystem::create_directories(cache_location_);
                Poco::Logger::get("DiskCache").information("Created cache directory: " + cache_location_.string());
            }
            else
            {
                // Clear existing cache on initialization
                Poco::Logger::get("DiskCache").information("Clearing existing cache directory: " + cache_location_.string());
                // Clear cache without checking initialized_ flag since we're in the middle of initialization
                try
                {
                    size_t files_deleted = 0;
                    size_t bytes_freed = 0;

                    for (const auto &entry : std::filesystem::directory_iterator(cache_location_))
                    {
                        if (entry.is_regular_file())
                        {
                            bytes_freed += entry.file_size();
                            std::filesystem::remove(entry.path());
                            files_deleted++;
                        }
                    }

                    Poco::Logger::get("DiskCache").information("Cache cleared during initialization: " + std::to_string(files_deleted) + " files deleted, " + std::to_string(bytes_freed / (1024 * 1024)) + " MB freed");
                }
                catch (const std::exception &e)
                {
                    Poco::Logger::get("DiskCache").error("Failed to clear cache during initialization: " + std::string(e.what()));
                }
            }

            // Calculate current cache size (should be 0 after clear)
            current_size_bytes_ = calculateCacheSize();

            Poco::Logger::get("DiskCache").information("DiskCache initialized at: " + cache_location_.string() + " (Size: " + std::to_string(current_size_bytes_ / (1024 * 1024)) + " MB / " + std::to_string(size_limit_bytes_ / (1024 * 1024)) + " MB)");

            // Subscribe to configuration changes
            config_manager_->subscribeToConfigChanges([this](const ConfigChangeEvent &event)
                                                      {
                if (event.key == CONFIG_CACHE_LOCATION)
                {
                    try {
                        std::string new_location = std::any_cast<std::string>(event.new_value);
                        this->onCacheLocationChanged(new_location);
                    } catch (...) {}
                }
                else if (event.key == CONFIG_CACHE_SIZE_LIMIT_MB)
                {
                    try {
                        int new_limit_mb = std::any_cast<int>(event.new_value);
                        this->onSizeLimitChanged(new_limit_mb);
                    } catch (...) {}
                } });

            initialized_ = true;
            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("DiskCache").error("Failed to initialize DiskCache: " + std::string(e.what()));
            return false;
        }
    }

    void DiskCache::shutdown()
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);

        if (!initialized_)
        {
            return;
        }

        Poco::Logger::get("DiskCache").information("DiskCache shutting down");
        initialized_ = false;
    }

    bool DiskCache::copyToCache(const std::string &source_path, std::string &cached_path)
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);

        if (!initialized_)
        {
            Poco::Logger::get("DiskCache").error("DiskCache not initialized");
            return false;
        }

        try
        {
            // Check if source file exists
            if (!std::filesystem::exists(source_path))
            {
                Poco::Logger::get("DiskCache").error("Source file not found: " + source_path);
                return false;
            }

            // Get file size
            size_t file_size = std::filesystem::file_size(source_path);

            // Enforce size limit
            enforceSizeLimit(file_size);

            // Generate cache filename
            std::string cache_filename = generateCacheFilename(source_path);
            std::filesystem::path dest_path = cache_location_ / cache_filename;
            std::string dest_path_str = dest_path.string();

            // Check if file is currently in use
            if (files_in_use_.find(dest_path_str) != files_in_use_.end())
            {
                Poco::Logger::get("DiskCache").warning("Cannot overwrite file currently in use: " + dest_path_str);
                return false;
            }

            // Copy file (overwrite existing)
            std::filesystem::copy_file(source_path, dest_path,
                                       std::filesystem::copy_options::overwrite_existing);

            // Update cache size
            current_size_bytes_ += file_size;

            cached_path = dest_path.string();

            Poco::Logger::get("DiskCache").debug("Copied to cache: " + source_path + " -> " + cached_path + " (" + std::to_string(file_size / 1024) + " KB)");

            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("DiskCache").error("Failed to copy to cache: " + std::string(e.what()));
            return false;
        }
    }

    bool DiskCache::saveStreamToCache(std::istream &stream, const std::string &filename, std::string &cached_path)
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);

        if (!initialized_)
        {
            Poco::Logger::get("DiskCache").error("DiskCache not initialized");
            return false;
        }

        try
        {
            // Generate cache filename
            std::string cache_filename = generateCacheFilename(filename);
            std::filesystem::path dest_path = cache_location_ / cache_filename;
            std::string dest_path_str = dest_path.string();

            // Check if file is currently in use
            if (files_in_use_.find(dest_path_str) != files_in_use_.end())
            {
                Poco::Logger::get("DiskCache").warning("Cannot overwrite file currently in use: " + dest_path_str);
                return false;
            }

            // Write stream to temporary file first to get size
            std::filesystem::path temp_path = cache_location_ / (cache_filename + ".tmp");

            {
                std::ofstream out_file(temp_path, std::ios::binary);
                if (!out_file)
                {
                    Poco::Logger::get("DiskCache").error("Failed to create temporary file: " + temp_path.string());
                    return false;
                }

                out_file << stream.rdbuf();
            }

            // Get file size
            size_t file_size = std::filesystem::file_size(temp_path);

            // Enforce size limit
            enforceSizeLimit(file_size);

            // Rename temp file to final name
            std::filesystem::rename(temp_path, dest_path);

            // Update cache size
            current_size_bytes_ += file_size;

            cached_path = dest_path.string();

            Poco::Logger::get("DiskCache").debug("Saved stream to cache: " + filename + " -> " + cached_path + " (" + std::to_string(file_size / 1024) + " KB)");

            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("DiskCache").error("Failed to save stream to cache: " + std::string(e.what()));
            return false;
        }
    }

    bool DiskCache::deleteFromCache(const std::string &cached_path)
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);

        if (!initialized_)
        {
            Poco::Logger::get("DiskCache").error("DiskCache not initialized");
            return false;
        }

        try
        {
            std::filesystem::path path(cached_path);

            // Verify the file is in the cache directory
            if (path.string().find(cache_location_.string()) != 0)
            {
                Poco::Logger::get("DiskCache").error("File is not in cache directory: " + cached_path);
                return false;
            }

            if (!std::filesystem::exists(path))
            {
                Poco::Logger::get("DiskCache").warning("File not found in cache: " + cached_path);
                return false;
            }

            // Get file size before deletion
            size_t file_size = std::filesystem::file_size(path);

            // Delete file
            std::filesystem::remove(path);

            // Update cache size
            current_size_bytes_ -= file_size;

            Poco::Logger::get("DiskCache").debug("Deleted from cache: " + cached_path);

            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("DiskCache").error("Failed to delete from cache: " + std::string(e.what()));
            return false;
        }
    }

    bool DiskCache::clearCache()
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);

        if (!initialized_)
        {
            Poco::Logger::get("DiskCache").error("DiskCache not initialized");
            return false;
        }

        try
        {
            size_t files_deleted = 0;
            size_t bytes_freed = 0;

            for (const auto &entry : std::filesystem::directory_iterator(cache_location_))
            {
                if (entry.is_regular_file())
                {
                    bytes_freed += entry.file_size();
                    std::filesystem::remove(entry.path());
                    files_deleted++;
                }
            }

            current_size_bytes_ = 0;

            Poco::Logger::get("DiskCache").information("Cache cleared: " + std::to_string(files_deleted) + " files deleted, " + std::to_string(bytes_freed / (1024 * 1024)) + " MB freed");

            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("DiskCache").error("Failed to clear cache: " + std::string(e.what()));
            return false;
        }
    }

    size_t DiskCache::getCurrentSizeMB() const
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        return current_size_bytes_ / (1024 * 1024);
    }

    size_t DiskCache::getSizeLimitMB() const
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        return size_limit_bytes_ / (1024 * 1024);
    }

    std::string DiskCache::getCacheLocation() const
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        return cache_location_.string();
    }

    void DiskCache::enforceSizeLimit(size_t required_space_bytes)
    {
        // If file is larger than limit, clear cache and log warning
        if (required_space_bytes > size_limit_bytes_)
        {
            Poco::Logger::get("DiskCache").warning("File size (" + std::to_string(required_space_bytes / (1024 * 1024)) + " MB) exceeds cache limit (" + std::to_string(size_limit_bytes_ / (1024 * 1024)) + " MB). Clearing cache to accommodate.");

            // Clear entire cache
            for (const auto &entry : std::filesystem::directory_iterator(cache_location_))
            {
                if (entry.is_regular_file())
                {
                    std::filesystem::remove(entry.path());
                }
            }
            current_size_bytes_ = 0;
            return;
        }

        // Calculate space needed
        size_t available_space = (current_size_bytes_ < size_limit_bytes_)
                                     ? (size_limit_bytes_ - current_size_bytes_)
                                     : 0;

        if (required_space_bytes > available_space)
        {
            size_t bytes_to_free = required_space_bytes - available_space;
            removeOldestFiles(bytes_to_free);
        }
    }

    void DiskCache::removeOldestFiles(size_t bytes_to_free)
    {
        auto files_by_age = getFilesByAge();

        size_t bytes_freed = 0;
        size_t files_removed = 0;

        for (const auto &[path, time] : files_by_age)
        {
            if (bytes_freed >= bytes_to_free)
            {
                break;
            }

            try
            {
                size_t file_size = std::filesystem::file_size(path);
                std::filesystem::remove(path);
                bytes_freed += file_size;
                current_size_bytes_ -= file_size;
                files_removed++;
            }
            catch (const std::exception &e)
            {
                Poco::Logger::get("DiskCache").error("Failed to remove file: " + path.string() + " - " + e.what());
            }
        }

        Poco::Logger::get("DiskCache").information("Removed " + std::to_string(files_removed) + " oldest files, freed " + std::to_string(bytes_freed / (1024 * 1024)) + " MB");
    }

    size_t DiskCache::calculateCacheSize()
    {
        size_t total_size = 0;

        try
        {
            if (!std::filesystem::exists(cache_location_))
            {
                return 0;
            }

            for (const auto &entry : std::filesystem::directory_iterator(cache_location_))
            {
                if (entry.is_regular_file())
                {
                    total_size += entry.file_size();
                }
            }
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("DiskCache").error("Failed to calculate cache size: " + std::string(e.what()));
        }

        return total_size;
    }

    std::vector<std::pair<std::filesystem::path, std::filesystem::file_time_type>> DiskCache::getFilesByAge()
    {
        std::vector<std::pair<std::filesystem::path, std::filesystem::file_time_type>> files;

        try
        {
            for (const auto &entry : std::filesystem::directory_iterator(cache_location_))
            {
                if (entry.is_regular_file())
                {
                    files.emplace_back(entry.path(), std::filesystem::last_write_time(entry.path()));
                }
            }

            // Sort by time (oldest first) - FIFO
            std::sort(files.begin(), files.end(),
                      [](const auto &a, const auto &b)
                      {
                          return a.second < b.second;
                      });
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("DiskCache").error("Failed to get files by age: " + std::string(e.what()));
        }

        return files;
    }

    std::string DiskCache::generateCacheFilename(const std::string &original_path)
    {
        // Extract filename from path (use original filename directly)
        std::filesystem::path path(original_path);
        return path.filename().string();
    }

    void DiskCache::onCacheLocationChanged(const std::string &new_location)
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);

        Poco::Logger::get("DiskCache").information("Cache location changed from " + cache_location_.string() + " to " + new_location);

        try
        {
            // Update location
            cache_location_ = std::filesystem::current_path() / new_location;

            // Create new directory
            if (!std::filesystem::exists(cache_location_))
            {
                std::filesystem::create_directories(cache_location_);
            }

            // Recalculate cache size for new location
            current_size_bytes_ = calculateCacheSize();

            Poco::Logger::get("DiskCache").information("Cache relocated successfully. New size: " + std::to_string(current_size_bytes_ / (1024 * 1024)) + " MB");
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("DiskCache").error("Failed to change cache location: " + std::string(e.what()));
        }
    }

    void DiskCache::onSizeLimitChanged(int new_limit_mb)
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);

        size_t old_limit_mb = size_limit_bytes_ / (1024 * 1024);
        size_t new_limit_bytes = new_limit_mb * 1024ULL * 1024ULL;

        Poco::Logger::get("DiskCache").information("Cache size limit changed from " + std::to_string(old_limit_mb) + " MB to " + std::to_string(new_limit_mb) + " MB");

        size_limit_bytes_ = new_limit_bytes;

        // If current size exceeds new limit, remove oldest files
        if (current_size_bytes_ > size_limit_bytes_)
        {
            size_t bytes_to_free = current_size_bytes_ - size_limit_bytes_;
            Poco::Logger::get("DiskCache").information("Current cache size exceeds new limit. Freeing " + std::to_string(bytes_to_free / (1024 * 1024)) + " MB");
            removeOldestFiles(bytes_to_free);
        }
    }

    void DiskCache::markFileInUse(const std::string &cached_path)
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        files_in_use_.insert(cached_path);
        Poco::Logger::get("DiskCache").debug("Marked file as in use: " + cached_path);
    }

    void DiskCache::markFileNotInUse(const std::string &cached_path)
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        files_in_use_.erase(cached_path);
        Poco::Logger::get("DiskCache").debug("Marked file as not in use: " + cached_path);
    }

    bool DiskCache::deleteFromCacheImmediately(const std::string &cached_path)
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);

        if (!initialized_)
        {
            Poco::Logger::get("DiskCache").error("DiskCache not initialized");
            return false;
        }

        // Check if file is currently in use
        if (files_in_use_.find(cached_path) != files_in_use_.end())
        {
            Poco::Logger::get("DiskCache").warning("Cannot delete file currently in use: " + cached_path);
            return false;
        }

        try
        {
            std::filesystem::path file_path(cached_path);

            // Check if file exists first
            if (!std::filesystem::exists(file_path))
            {
                Poco::Logger::get("DiskCache").debug("File not found in cache (already deleted): " + cached_path);
                return true; // Consider it successfully deleted
            }

            // Verify file is within cache directory for security
            std::filesystem::path canonical_file_path = std::filesystem::canonical(file_path);
            std::filesystem::path canonical_cache_path = std::filesystem::canonical(cache_location_);

            if (!std::filesystem::equivalent(canonical_file_path.parent_path(), canonical_cache_path))
            {
                Poco::Logger::get("DiskCache").error("File path outside cache directory: " + cached_path);
                return false;
            }

            auto file_size = std::filesystem::file_size(file_path);
            std::filesystem::remove(file_path);

            // Update current size
            if (current_size_bytes_ >= file_size)
            {
                current_size_bytes_ -= file_size;
            }
            else
            {
                current_size_bytes_ = 0; // Should not happen, but be safe
            }

            Poco::Logger::get("DiskCache").debug("Deleted file from cache: " + cached_path + " (freed " + std::to_string(file_size / (1024 * 1024)) + " MB)");
            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("DiskCache").error("Error deleting file from cache " + cached_path + ": " + e.what());
            return false;
        }
    }

} // namespace MediaDedup
