#pragma once

#include <string>
#include <optional>
#include <cstdint>

namespace MediaDedup
{
    class DatabaseManager;

    /**
     * @brief Record for a cached thumbnail
     */
    struct ThumbnailCacheRecord
    {
        int id = 0;
        std::string source_path;
        std::string cached_path;
        int thumbnail_size = 256;
        int64_t file_size_bytes = 0;
        int64_t source_modified_at = 0; // Unix timestamp
        int64_t created_at = 0;         // Unix timestamp
        int64_t last_accessed_at = 0;   // Unix timestamp
    };

    /**
     * @brief Operations for thumbnail cache database table
     */
    class ThumbnailCacheOps
    {
    public:
        /**
         * @brief Ensure thumbnail_cache table and indices exist
         * @param db Database manager
         * @return true if successful, false otherwise
         */
        static bool ensureTable(DatabaseManager &db);

        /**
         * @brief Get thumbnail record for a source path and size
         * @param db Database manager
         * @param source_path Source file path
         * @param size Thumbnail size (128, 256, 512, or 1024)
         * @return Optional record if found
         */
        static std::optional<ThumbnailCacheRecord> getThumbnail(DatabaseManager &db,
                                                                const std::string &source_path,
                                                                int size);

        // lookup_failed (optional) is set true when the query itself failed
        // (e.g. DB session pool starved) — callers must treat that as "try
        // again", NOT as a cache miss, or they'd re-submit generation for an
        // asset that is already cached.
        static std::optional<ThumbnailCacheRecord> getBestFitThumbnail(DatabaseManager &db,
                                                                       const std::string &source_path,
                                                                       int min_size,
                                                                       bool *lookup_failed = nullptr);

        /**
         * @brief Insert or update a thumbnail cache record
         * @param db Database manager
         * @param record Thumbnail record to upsert
         * @return true if successful, false otherwise
         */
        static bool upsertThumbnail(DatabaseManager &db, const ThumbnailCacheRecord &record);

        /**
         * @brief Update last accessed timestamp for a thumbnail
         * @param db Database manager
         * @param source_path Source file path
         * @param size Thumbnail size
         * @param timestamp Unix timestamp
         * @return true if successful, false otherwise
         */
        static bool updateAccessTime(DatabaseManager &db,
                                     const std::string &source_path,
                                     int size,
                                     int64_t timestamp);

        /**
         * @brief Delete thumbnail cache entry
         * @param db Database manager
         * @param source_path Source file path
         * @param size Thumbnail size (if 0, deletes all sizes for this path)
         * @return true if successful, false otherwise
         */
        static bool deleteThumbnail(DatabaseManager &db,
                                    const std::string &source_path,
                                    int size = 0);

        /**
         * @brief Delete stale thumbnails (source file modified after cache entry)
         * @param db Database manager
         * @param before_timestamp Delete entries where source_modified_at < this timestamp
         * @return Number of entries deleted
         */
        static int deleteStaleEntries(DatabaseManager &db, int64_t before_timestamp);

        /**
         * @brief Get total count of cached thumbnails
         * @param db Database manager
         * @return Count of thumbnail cache entries
         */
        static int getCount(DatabaseManager &db);

        /**
         * @brief Clears all entries from the thumbnail cache
         * @param db Database manager
         * @return true if successful, false otherwise
         */
        static bool clearCache(DatabaseManager &db);

        /**
         * @brief Update the base path for all cached thumbnails (used when moving cache location)
         * @param db Database manager
         * @param old_base_path Old absolute base path
         * @param new_base_path New absolute base path
         * @return true if successful, false otherwise
         */
        static bool updateCacheLocation(DatabaseManager &db, const std::string &old_base_path, const std::string &new_base_path);
    };

} // namespace MediaDedup
