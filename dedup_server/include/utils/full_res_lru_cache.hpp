#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace MediaDedup
{
    // ADJ-1: render_revision is part of the identity — an edited file must
    // never be served its pre-edit native render.
    struct FullResEntry { std::string temp_path; int64_t source_mtime; std::size_t bytes = 0; int render_revision = 0; };

    /**
     * @brief MP-4: bounded LRU for the viewer's full-resolution temp renders.
     *
     * The previous unbounded map never deleted its quality-95 native-res
     * temp JPEGs during the process lifetime (macOS only clears /var/folders
     * on reboot) — scrolling the viewer through 500 photos leaked ~5 GB
     * invisible to every cache limit. Entry + byte caps; eviction, staleness
     * replacement, clear(), and destruction all unlink the temp file.
     */
    class FullResLruCache {
    public:
        static constexpr std::size_t kMaxEntries = 32;
        static constexpr std::size_t kMaxBytes = 512ULL * 1024 * 1024;

        explicit FullResLruCache(std::size_t max_entries = kMaxEntries,
                                 std::size_t max_bytes = kMaxBytes)
            : max_entries_(max_entries), max_bytes_(max_bytes) {}

        // Fresh hit: file still on disk and mtime matches (or unknown). Touches
        // LRU recency. A stale entry is dropped AND its temp file unlinked (the
        // old code leaked the file on staleness).
        std::optional<FullResEntry> getFresh(const std::string& key, int64_t current_mtime, int render_revision = 0) {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = map_.find(key);
            if (it == map_.end())
                return std::nullopt;
            const FullResEntry& e = it->second.first;
            if (e.render_revision == render_revision && std::filesystem::exists(e.temp_path) &&
                (current_mtime == 0 || e.source_mtime == 0 || e.source_mtime == current_mtime)) {
                lru_.splice(lru_.begin(), lru_, it->second.second); // touch
                return e;
            }
            eraseLocked(it, /*unlink=*/true);
            return std::nullopt;
        }

        void put(const std::string& key, FullResEntry entry) {
            std::error_code ec;
            auto sz = std::filesystem::file_size(entry.temp_path, ec);
            entry.bytes = ec ? 0 : static_cast<std::size_t>(sz);

            std::lock_guard<std::mutex> lock(mutex_);
            auto it = map_.find(key);
            if (it != map_.end())
                eraseLocked(it, /*unlink=*/true); // replacing: drop the old render
            lru_.push_front(key);
            total_bytes_ += entry.bytes;
            map_.emplace(key, std::make_pair(std::move(entry), lru_.begin()));

            while (map_.size() > max_entries_ || total_bytes_ > max_bytes_) {
                auto victim = map_.find(lru_.back());
                if (victim == map_.end()) { lru_.pop_back(); continue; }
                eraseLocked(victim, /*unlink=*/true);
            }
        }

        // ADJ-1: drop one file's render (its edits changed).
        void erase(const std::string& key) {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = map_.find(key);
            if (it != map_.end())
                eraseLocked(it, /*unlink=*/true);
        }

        // Deletes all temp renders and empties the index (QW-9 clear-caches).
        void clear() {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& [key, value] : map_) {
                std::error_code ec;
                std::filesystem::remove(value.first.temp_path, ec);
            }
            map_.clear();
            lru_.clear();
            total_bytes_ = 0;
        }

        ~FullResLruCache() { clear(); } // shutdown: no native-res temp litter

    private:
        using MapValue = std::pair<FullResEntry, std::list<std::string>::iterator>;

        void eraseLocked(std::unordered_map<std::string, MapValue>::iterator it, bool unlink) {
            if (unlink) {
                std::error_code ec;
                std::filesystem::remove(it->second.first.temp_path, ec);
            }
            total_bytes_ -= std::min(total_bytes_, it->second.first.bytes);
            lru_.erase(it->second.second);
            map_.erase(it);
        }

        std::mutex mutex_;
        std::size_t max_entries_;
        std::size_t max_bytes_;
        std::list<std::string> lru_; // most recent at front
        std::unordered_map<std::string, MapValue> map_;
        std::size_t total_bytes_ = 0;
    };

} // namespace MediaDedup
