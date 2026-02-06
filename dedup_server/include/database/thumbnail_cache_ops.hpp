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
    };

} // namespace MediaDedup
