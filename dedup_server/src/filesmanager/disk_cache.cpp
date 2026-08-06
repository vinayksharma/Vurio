#include "filesmanager/disk_cache.hpp"
#include <Poco/Logger.h>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <functional>
#include <filesystem>
#include <mutex>
#include <set>

namespace MediaDedup
{
    namespace
    {
        Poco::Logger &cacheLogger()
        {
            static Poco::Logger &logger = Poco::Logger::get("DiskCache");
            return logger;
        }

        // Lexical containment check: is `path` inside `root`?
        bool isUnderDirectory(const std::filesystem::path &path, const std::filesystem::path &root)
        {
            auto norm_path = path.lexically_normal();
            auto norm_root = root.lexically_normal();
            auto rel = norm_path.lexically_relative(norm_root);
            return !rel.empty() && rel.native().rfind("..", 0) != 0;
        }
    }

    DiskCache::DiskCache(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                         const std::string &config_prefix)
        : config_manager_(config_manager),
          cache_location_(DEFAULT_CACHE_LOCATION),
          size_limit_bytes_(DEFAULT_CACHE_SIZE_LIMIT_MB * 1024ULL * 1024ULL),
          current_size_bytes_(0),
          initialized_(false),
          config_prefix_(config_prefix)
    {
        // Construct config keys from prefix
        config_key_size_limit_ = config_prefix_ + ".size_limit_mb";
        config_key_clear_on_startup_ = config_prefix_ + ".clearOnStartup";
    }

    DiskCache::~DiskCache()
    {
        shutdown();
    }

    void DiskCache::setEvictionHooks(LruQueryFn lru_query, EvictedNotifyFn evicted_notify)
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        lru_query_ = std::move(lru_query);
        evicted_notify_ = std::move(evicted_notify);
    }

    bool DiskCache::initialize()
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);

        if (initialized_)
        {
            cacheLogger().warning("DiskCache already initialized");
            return true;
        }

        try
        {
            // Get cache location from config
            std::string config_key_location = config_prefix_ + ".location";
            std::string location = config_manager_->getPropertyValue<std::string>(config_key_location, "./" + config_prefix_);

            std::filesystem::path base_path(location);
            if (!base_path.is_absolute()) {
                base_path = std::filesystem::current_path() / base_path;
            }

            cache_location_ = base_path;

            // Get size limit from config
            int size_limit_mb = config_manager_->getPropertyValue<int>(
                config_key_size_limit_, DEFAULT_CACHE_SIZE_LIMIT_MB);
            size_limit_bytes_ = size_limit_mb * 1024ULL * 1024ULL;

            // Check if directory exists before trying to clear it
            if (std::filesystem::exists(cache_location_))
            {
                // Check if cache should be cleared on startup (configurable per cache type)
                bool clear_on_startup = config_manager_->getPropertyValue<bool>(config_key_clear_on_startup_, false);

                if (clear_on_startup)
                {
                    // Clear existing cache on initialization (for temporary caches like transcoding)
                    cacheLogger().information("Clearing existing cache directory: " + cache_location_.string());
                    try
                    {
                        size_t files_deleted = 0;
                        size_t bytes_freed = 0;

                        for (const auto &entry : std::filesystem::recursive_directory_iterator(cache_location_))
                        {
                            if (entry.is_regular_file())
                            {
                                bytes_freed += entry.file_size();
                                std::filesystem::remove(entry.path());
                                files_deleted++;
                            }
                        }

                        cacheLogger().information("Cache cleared during initialization: " + std::to_string(files_deleted) + " files deleted, " + std::to_string(bytes_freed / (1024 * 1024)) + " MB freed");
                    }
                    catch (const std::exception &e)
                    {
                        cacheLogger().error("Failed to clear cache during initialization: " + std::string(e.what()));
                    }
                }
                else
                {
                    // Cache directory exists - preserve existing cache files (for persistent caches like thumbnails)
                    cacheLogger().information("Using existing cache directory: " + cache_location_.string());
                }
            }
            else
            {
                // Create directory if it doesn't exist so the user can easily find it
                cacheLogger().information("Creating cache directory on initialization: " + cache_location_.string());
                std::filesystem::create_directories(cache_location_);
            }

            // MP-2: the ONE full scan — every later size question is answered
            // from the in-memory index.
            rebuildIndexLocked();

            cacheLogger().information("DiskCache initialized at: " + cache_location_.string() + " (Size: " + std::to_string(current_size_bytes_ / (1024 * 1024)) + " MB / " + std::to_string(size_limit_bytes_ / (1024 * 1024)) + " MB, " + std::to_string(index_.size()) + " files)");

            // Enforce limit on startup if cache is already overgrown
            if (current_size_bytes_ > size_limit_bytes_)
            {
                size_t bytes_to_free = current_size_bytes_ - size_limit_bytes_;
                cacheLogger().information("Startup check: Cache size exceeds limit. Freeing " + std::to_string(bytes_to_free / (1024 * 1024)) + " MB");
                removeOldestFilesLocked(bytes_to_free);
            }

            // Subscribe to configuration changes
            config_subscription_ = ScopedConfigSubscription(
                config_manager_,
                config_manager_->subscribeToConfigChanges([this](const ConfigChangeEvent &event)
                                                      {
                std::string config_key_location = this->config_prefix_ + ".location";
                if (event.key == config_key_location)
                {
                    try {
                        std::string new_location = std::any_cast<std::string>(event.new_value);
                        this->onCacheLocationChanged(new_location);
                    } catch (...) {}
                }
                else if (event.key == this->config_key_size_limit_)
                {
                    try {
                        int new_limit_mb = std::any_cast<int>(event.new_value);
                        this->onSizeLimitChanged(new_limit_mb);
                    } catch (...) {}
                } }));

            initialized_ = true;
            return true;
        }
        catch (const std::exception &e)
        {
            cacheLogger().error("Failed to initialize DiskCache: " + std::string(e.what()));
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

        cacheLogger().information("DiskCache shutting down");
        initialized_ = false;
    }

    bool DiskCache::copyToCache(const std::string &source_path, std::string &cached_path)
    {
        try
        {
            // Source inspection happens before any locking.
            if (!std::filesystem::exists(source_path))
            {
                cacheLogger().error("Source file not found: " + source_path);
                return false;
            }
            size_t file_size = std::filesystem::file_size(source_path);

            std::filesystem::path dest_path;
            std::string dest_path_str;

            // Phase 1 (locked): admission, eviction, claim the destination.
            {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                if (!initialized_)
                {
                    cacheLogger().error("DiskCache not initialized");
                    return false;
                }
                if (file_size > size_limit_bytes_)
                {
                    cacheLogger().warning("File size (" + std::to_string(file_size / (1024 * 1024)) + " MB) exceeds total cache limit (" + std::to_string(size_limit_bytes_ / (1024 * 1024)) + " MB). Refusing to cache.");
                    return false;
                }

                enforceSizeLimitLocked(file_size);

                dest_path = cache_location_ / generateCacheFilename(source_path);
                dest_path_str = dest_path.string();

                if (isBusyLocked(dest_path_str))
                {
                    cacheLogger().warning("Cannot overwrite file currently in use: " + dest_path_str);
                    return false;
                }
                // Re-caching the same source: uncount the entry being replaced.
                indexEraseLocked(dest_path_str);
                pending_writes_.insert(dest_path_str);
            }

            // Phase 2 (UNLOCKED): the multi-MB copy no longer stalls every
            // other cache user (previously the effective throughput ceiling).
            bool copied = false;
            try
            {
                std::filesystem::create_directories(dest_path.parent_path());
                std::filesystem::copy_file(source_path, dest_path,
                                           std::filesystem::copy_options::overwrite_existing);
                copied = true;
            }
            catch (const std::exception &e)
            {
                cacheLogger().error("Failed to copy to cache: " + std::string(e.what()));
            }

            // Phase 3 (locked): account the result, release the claim.
            {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                pending_writes_.erase(dest_path_str);
                if (copied)
                    indexInsertLocked(dest_path_str, file_size);
            }

            if (!copied)
                return false;

            cached_path = dest_path_str;
            cacheLogger().debug("Copied to cache: " + source_path + " -> " + cached_path + " (" + std::to_string(file_size / 1024) + " KB)");
            return true;
        }
        catch (const std::exception &e)
        {
            cacheLogger().error("Failed to copy to cache: " + std::string(e.what()));
            return false;
        }
    }

    bool DiskCache::moveToCache(const std::string &source_path, std::string &cached_path)
    {
        try
        {
            if (!std::filesystem::exists(source_path))
            {
                cacheLogger().error("Source file not found: " + source_path);
                return false;
            }
            size_t file_size = std::filesystem::file_size(source_path);

            std::filesystem::path dest_path;
            std::string dest_path_str;

            {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                if (!initialized_)
                {
                    cacheLogger().error("DiskCache not initialized");
                    return false;
                }
                if (file_size > size_limit_bytes_)
                {
                    cacheLogger().warning("File size exceeds total cache limit. Refusing to cache.");
                    return false;
                }

                enforceSizeLimitLocked(file_size);

                dest_path = cache_location_ / generateCacheFilename(source_path);
                dest_path_str = dest_path.string();

                if (isBusyLocked(dest_path_str))
                {
                    cacheLogger().warning("Cannot overwrite file currently in use: " + dest_path_str);
                    return false;
                }
                indexEraseLocked(dest_path_str);
                pending_writes_.insert(dest_path_str);
            }

            bool moved = false;
            try
            {
                std::filesystem::create_directories(dest_path.parent_path());
                std::filesystem::rename(source_path, dest_path);
                moved = true;
            }
            catch (const std::exception &e)
            {
                cacheLogger().error("Failed to move to cache: " + std::string(e.what()));
            }

            {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                pending_writes_.erase(dest_path_str);
                if (moved)
                    indexInsertLocked(dest_path_str, file_size);
            }

            if (!moved)
                return false;

            cached_path = dest_path_str;
            cacheLogger().debug("Moved to cache: " + source_path + " -> " + cached_path);
            return true;
        }
        catch (const std::exception &e)
        {
            cacheLogger().error("Failed to move to cache: " + std::string(e.what()));
            return false;
        }
    }

    bool DiskCache::saveStreamToCache(std::istream &stream, const std::string &filename, std::string &cached_path)
    {
        try
        {
            std::filesystem::path dest_path;
            std::string dest_path_str;
            std::filesystem::path temp_path;

            // Phase 1 (locked): claim the destination before writing.
            {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                if (!initialized_)
                {
                    cacheLogger().error("DiskCache not initialized");
                    return false;
                }

                dest_path = cache_location_ / generateCacheFilename(filename);
                dest_path_str = dest_path.string();

                if (isBusyLocked(dest_path_str))
                {
                    cacheLogger().warning("Cannot overwrite file currently in use: " + dest_path_str);
                    return false;
                }
                pending_writes_.insert(dest_path_str);
            }

            temp_path = dest_path;
            temp_path += ".tmp";

            // Phase 2 (UNLOCKED): stream the payload to a temp file.
            size_t file_size = 0;
            bool written = false;
            try
            {
                std::filesystem::create_directories(dest_path.parent_path());
                {
                    std::ofstream out_file(temp_path, std::ios::binary);
                    if (out_file)
                    {
                        stream.clear();
                        stream.seekg(0, std::ios::beg);
                        if (stream.good())
                        {
                            out_file << stream.rdbuf();
                            out_file.flush();
                            written = out_file.good();
                        }
                    }
                }
                if (written)
                {
                    file_size = std::filesystem::file_size(temp_path);
                    if (file_size == 0)
                    {
                        cacheLogger().error("Stream resulted in empty file: " + temp_path.string());
                        written = false;
                    }
                }
                else
                {
                    cacheLogger().error("Failed to write stream data to temporary file: " + temp_path.string());
                }
            }
            catch (const std::exception &e)
            {
                cacheLogger().error("Failed to save stream to cache: " + std::string(e.what()));
                written = false;
            }

            bool finalized = false;
            {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                if (written && file_size > size_limit_bytes_)
                {
                    cacheLogger().warning("Stream size (" + std::to_string(file_size / (1024 * 1024)) + " MB) exceeds total cache limit (" + std::to_string(size_limit_bytes_ / (1024 * 1024)) + " MB). Refusing to cache.");
                    written = false;
                }
                if (written)
                {
                    enforceSizeLimitLocked(file_size);
                    indexEraseLocked(dest_path_str); // replacing an existing entry
                    try
                    {
                        std::filesystem::rename(temp_path, dest_path);
                        indexInsertLocked(dest_path_str, file_size);
                        finalized = true;
                    }
                    catch (const std::exception &e)
                    {
                        cacheLogger().error("Failed to finalize cached stream: " + std::string(e.what()));
                    }
                }
                pending_writes_.erase(dest_path_str);
            }

            if (!finalized)
            {
                std::error_code ec;
                std::filesystem::remove(temp_path, ec);
                return false;
            }

            cached_path = dest_path_str;
            cacheLogger().debug("Saved stream to cache: " + filename + " -> " + cached_path + " (" + std::to_string(file_size / 1024) + " KB)");
            return true;
        }
        catch (const std::exception &e)
        {
            cacheLogger().error("Failed to save stream to cache: " + std::string(e.what()));
            return false;
        }
    }

    bool DiskCache::deleteFromCache(const std::string &cached_path)
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);

        if (!initialized_)
        {
            cacheLogger().error("DiskCache not initialized");
            return false;
        }

        try
        {
            std::filesystem::path path(cached_path);

            // Verify the file is in the cache directory
            if (!isUnderDirectory(path, cache_location_))
            {
                cacheLogger().error("File is not in cache directory: " + cached_path);
                return false;
            }

            if (isBusyLocked(cached_path))
            {
                cacheLogger().warning("Cannot delete file currently in use: " + cached_path);
                return false;
            }

            if (!std::filesystem::exists(path))
            {
                indexEraseLocked(cached_path);
                return false;
            }

            std::filesystem::remove(path);
            indexEraseLocked(cached_path);

            cacheLogger().debug("Deleted from cache: " + cached_path);
            return true;
        }
        catch (const std::exception &e)
        {
            cacheLogger().error("Failed to delete from cache: " + std::string(e.what()));
            return false;
        }
    }

    bool DiskCache::clearCache()
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);

        if (!initialized_)
        {
            cacheLogger().error("DiskCache not initialized");
            return false;
        }

        try
        {
            size_t files_deleted = 0;
            size_t bytes_freed = 0;
            std::vector<std::string> deleted_paths;

            if (std::filesystem::exists(cache_location_)) {
                for (const auto &entry : std::filesystem::recursive_directory_iterator(cache_location_))
                {
                    if (entry.is_regular_file())
                    {
                        bytes_freed += entry.file_size();
                        deleted_paths.push_back(entry.path().string());
                        std::filesystem::remove(entry.path());
                        files_deleted++;
                    }
                }
            }

            index_.clear();
            current_size_bytes_ = 0;

            // Clear the files_in_use_ set since all files have been deleted
            files_in_use_.clear();

            // Keep the owning tables consistent with the emptied directory.
            if (evicted_notify_ && !deleted_paths.empty())
            {
                try { evicted_notify_(deleted_paths); } catch (...) {}
            }

            cacheLogger().information("Cache cleared: " + std::to_string(files_deleted) + " files deleted, " + std::to_string(bytes_freed / (1024 * 1024)) + " MB freed");

            return true;
        }
        catch (const std::exception &e)
        {
            cacheLogger().error("Failed to clear cache: " + std::string(e.what()));
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

    void DiskCache::enforceSizeLimitLocked(size_t required_space_bytes)
    {
        size_t projected_size = current_size_bytes_ + required_space_bytes;
        if (projected_size <= size_limit_bytes_)
            return;

        // MP-2: batch to 10% headroom below the limit so eviction doesn't
        // re-trigger on every subsequent insert.
        size_t bytes_to_free = (projected_size - size_limit_bytes_) + size_limit_bytes_ / 10;
        removeOldestFilesLocked(bytes_to_free);
    }

    void DiskCache::removeOldestFilesLocked(size_t bytes_to_free)
    {
        size_t bytes_freed = 0;
        size_t files_removed = 0;
        std::vector<std::string> deleted_paths;

        auto delete_one = [&](const std::string &path_str) -> bool
        {
            auto it = index_.find(path_str);
            if (it == index_.end())
                return false;
            if (isBusyLocked(path_str))
                return false;
            std::error_code ec;
            std::filesystem::remove(path_str, ec);
            if (ec)
            {
                cacheLogger().error("Failed to remove file: " + path_str + " - " + ec.message());
                return false;
            }
            bytes_freed += it->second.size;
            indexEraseLocked(path_str);
            deleted_paths.push_back(path_str);
            files_removed++;
            return true;
        };

        // 1) DB-driven LRU: the owning table knows real access recency.
        if (lru_query_)
        {
            try
            {
                // Estimate how many rows cover the target, with slack for
                // rows that are busy/missing.
                size_t avg = index_.empty() ? 0 : current_size_bytes_ / index_.size();
                size_t want = avg ? (bytes_to_free / avg) * 3 / 2 + 16 : 256;
                want = std::min<size_t>(std::max<size_t>(want, 16), 4096);

                for (const auto &candidate : lru_query_(want))
                {
                    if (bytes_freed >= bytes_to_free)
                        break;
                    delete_one(candidate);
                }
            }
            catch (const std::exception &e)
            {
                cacheLogger().warning("LRU eviction query failed, using insert-order fallback: " + std::string(e.what()));
            }
        }

        // 2) Fallback / remainder: index entries oldest-inserted first.
        if (bytes_freed < bytes_to_free)
        {
            std::vector<std::pair<std::string, std::chrono::steady_clock::time_point>> by_age;
            by_age.reserve(index_.size());
            for (const auto &kv : index_)
                by_age.emplace_back(kv.first, kv.second.inserted_at);
            std::sort(by_age.begin(), by_age.end(),
                      [](const auto &a, const auto &b) { return a.second < b.second; });

            for (const auto &[path_str, when] : by_age)
            {
                if (bytes_freed >= bytes_to_free)
                    break;
                delete_one(path_str);
            }
        }

        // Tell the owner which rows just lost their files — dangling rows
        // caused regeneration storms.
        if (evicted_notify_ && !deleted_paths.empty())
        {
            try { evicted_notify_(deleted_paths); } catch (...) {}
        }

        cacheLogger().information("Evicted " + std::to_string(files_removed) + " file(s), freed " + std::to_string(bytes_freed / (1024 * 1024)) + " MB" + (lru_query_ ? " (LRU)" : " (insert-order)"));
    }

    void DiskCache::rebuildIndexLocked()
    {
        index_.clear();
        current_size_bytes_ = 0;

        try
        {
            if (!std::filesystem::exists(cache_location_))
                return;

            const auto now = std::chrono::steady_clock::now();
            for (const auto &entry : std::filesystem::recursive_directory_iterator(cache_location_))
            {
                if (!entry.is_regular_file())
                    continue;
                IndexEntry ie;
                ie.size = entry.file_size();
                // Preserve on-disk age ordering for the fallback eviction:
                // rank existing files by mtime, mapped into steady_clock.
                std::error_code ec;
                auto mtime = std::filesystem::last_write_time(entry.path(), ec);
                if (!ec)
                {
                    auto age = std::filesystem::file_time_type::clock::now() - mtime;
                    ie.inserted_at = now - std::chrono::duration_cast<std::chrono::steady_clock::duration>(age);
                }
                else
                {
                    ie.inserted_at = now;
                }
                current_size_bytes_ += ie.size;
                index_.emplace(entry.path().string(), ie);
            }
        }
        catch (const std::exception &e)
        {
            cacheLogger().error("Failed to index cache directory: " + std::string(e.what()));
        }
    }

    void DiskCache::indexInsertLocked(const std::string &path, std::size_t size)
    {
        auto [it, inserted] = index_.insert_or_assign(path, IndexEntry{size, std::chrono::steady_clock::now()});
        (void)it;
        (void)inserted;
        current_size_bytes_ += size;
    }

    void DiskCache::indexEraseLocked(const std::string &path)
    {
        auto it = index_.find(path);
        if (it == index_.end())
            return;
        // Underflow guard: a wrapped counter previously projected the cache
        // as astronomically over-limit and nuked it.
        current_size_bytes_ = current_size_bytes_ >= it->second.size
                                  ? current_size_bytes_ - it->second.size
                                  : 0;
        index_.erase(it);
    }

    bool DiskCache::isBusyLocked(const std::string &path) const
    {
        return files_in_use_.count(path) > 0 || pending_writes_.count(path) > 0;
    }

    std::string DiskCache::generateCacheFilename(const std::string &original_path)
    {
        std::filesystem::path path(original_path);
        std::string stem = path.stem().string();
        std::string ext = path.extension().string();

        const std::size_t h = std::hash<std::string>{}(original_path);
        std::ostringstream hex;
        hex << std::hex << std::setfill('0') << std::setw(16) << static_cast<unsigned long long>(h);
        const std::string digest = hex.str();

        // "<2-hex shard>/<stem>_<digest><ext>": 256 shards keep directory
        // fan-out flat at scale; the full-path hash makes distinct sources
        // (same basename, different folders) collision-free.
        return digest.substr(0, 2) + "/" + stem + "_" + digest + ext;
    }

    void DiskCache::onCacheLocationChanged(const std::string &new_location)
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);

        cacheLogger().information("Cache location changed from " + cache_location_.string() + " to " + new_location);

        try
        {
            std::filesystem::path base_path(new_location);
            if (!base_path.is_absolute()) {
                base_path = std::filesystem::current_path() / base_path;
            }

            std::filesystem::path old_location = cache_location_;

            // Update location
            cache_location_ = base_path;

            // Create new directory
            if (!std::filesystem::exists(cache_location_))
            {
                std::filesystem::create_directories(cache_location_);
            }

            // Move contents from old location to new location (preserving
            // the shard-relative layout)
            if (!old_location.empty() && old_location != cache_location_ && std::filesystem::exists(old_location))
            {
                cacheLogger().information("Moving cache contents from " + old_location.string() + " to " + cache_location_.string());

                for (const auto& entry : std::filesystem::recursive_directory_iterator(old_location))
                {
                    if (entry.is_regular_file())
                    {
                        try {
                            auto rel = entry.path().lexically_relative(old_location);
                            auto dest = cache_location_ / rel;
                            std::filesystem::create_directories(dest.parent_path());
                            std::filesystem::rename(entry.path(), dest);
                        } catch (const std::exception& e) {
                            cacheLogger().warning("Failed to move file " + entry.path().string() + ": " + e.what());
                        }
                    }
                }

                // Try to remove old directory tree if empty
                try {
                    std::filesystem::remove_all(old_location);
                } catch (const std::exception& e) {
                    cacheLogger().warning("Failed to remove old cache directory: " + std::string(e.what()));
                }
            }

            // Re-index the new location
            rebuildIndexLocked();

            cacheLogger().information("Cache relocated successfully. New size: " + std::to_string(current_size_bytes_ / (1024 * 1024)) + " MB");
        }
        catch (const std::exception &e)
        {
            cacheLogger().error("Failed to change cache location: " + std::string(e.what()));
        }
    }

    void DiskCache::onSizeLimitChanged(int new_limit_mb)
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);

        size_t old_limit_mb = size_limit_bytes_ / (1024 * 1024);
        size_t new_limit_bytes = new_limit_mb * 1024ULL * 1024ULL;

        cacheLogger().information("Cache size limit changed from " + std::to_string(old_limit_mb) + " MB to " + std::to_string(new_limit_mb) + " MB");

        size_limit_bytes_ = new_limit_bytes;

        // If current size exceeds new limit, evict down to it
        if (current_size_bytes_ > size_limit_bytes_)
        {
            size_t bytes_to_free = current_size_bytes_ - size_limit_bytes_;
            cacheLogger().information("Current cache size exceeds new limit. Freeing " + std::to_string(bytes_to_free / (1024 * 1024)) + " MB");
            removeOldestFilesLocked(bytes_to_free);
        }
    }

    void DiskCache::markFileInUse(const std::string &cached_path)
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        files_in_use_.insert(cached_path);
        cacheLogger().debug("Marked file as in use: " + cached_path);
    }

    void DiskCache::markFileNotInUse(const std::string &cached_path)
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        files_in_use_.erase(cached_path);
        cacheLogger().debug("Marked file as not in use: " + cached_path);
    }

    bool DiskCache::deleteFromCacheImmediately(const std::string &cached_path)
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);

        if (!initialized_)
        {
            cacheLogger().error("DiskCache not initialized");
            return false;
        }

        // Check if file is currently in use (or mid-write)
        if (isBusyLocked(cached_path))
        {
            cacheLogger().warning("Cannot delete file currently in use: " + cached_path);
            return false;
        }

        try
        {
            std::filesystem::path file_path(cached_path);

            // Verify file is within the cache directory tree for security
            // (any shard depth — the old parent==root check broke sharding)
            if (!isUnderDirectory(file_path, cache_location_))
            {
                cacheLogger().error("File path outside cache directory: " + cached_path);
                return false;
            }

            // Check if file exists first
            if (!std::filesystem::exists(file_path))
            {
                indexEraseLocked(cached_path);
                cacheLogger().debug("File not found in cache (already deleted): " + cached_path);
                return true; // Consider it successfully deleted
            }

            std::filesystem::remove(file_path);
            indexEraseLocked(cached_path);

            cacheLogger().debug("Deleted file from cache: " + cached_path);
            return true;
        }
        catch (const std::exception &e)
        {
            cacheLogger().error("Error deleting file from cache " + cached_path + ": " + e.what());
            return false;
        }
    }

} // namespace MediaDedup
