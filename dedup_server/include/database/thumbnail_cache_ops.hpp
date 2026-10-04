#pragma once

#include <string>
#include <optional>
#include <cstdint>
#include <vector>

namespace MediaDedup
{
    class DatabaseManager;

    // THUMB-1: the render revision of an un-edited image. Every file is at
    // this revision until the non-destructive edit pipeline lands; it is part
    // of every thumbnail cache key (DB row, disk filename, client cache) so an
    // edited image can never be served its pre-edit bitmap. See
    // docs/todo_aspect_ratio_thumbnails.md §4.2(b).
    inline constexpr int kNoRenderRevision = 0;

    /**
     * @brief Record for a cached thumbnail
     */
    struct ThumbnailCacheRecord
    {
        int id = 0;
        std::string source_path;
        std::string cached_path;
        int thumbnail_size = 256;
        int render_revision = kNoRenderRevision; // THUMB-1
        int64_t file_size_bytes = 0;
        int64_t source_modified_at = 0; // Unix timestamp
        int64_t created_at = 0;         // Unix timestamp
        int64_t last_accessed_at = 0;   // Unix timestamp
    };

    /**
     * @brief STRIP-2: one cached bucket of a source file, as carried on the
     * day/duplicate list records (see VURIO_BEST_THUMB_SUBQUERY).
     */
    struct CachedThumbnailRef
    {
        std::string path;
        int size = 0;
    };

    /**
     * @brief Operations for thumbnail cache database table
     */
    class ThumbnailCacheOps
    {
    public:
        /**
         * @brief STRIP-2: decode the VURIO_BEST_THUMB_SUBQUERY column
         * ("<size>:<path>" entries joined by char(31)) into refs sorted by
         * size ascending. Malformed entries are skipped; empty input -> {}.
         */
        static std::vector<CachedThumbnailRef> parseCachedThumbnailList(const std::string &encoded);

        /**
         * @brief The path a client wanting `target` pixels should decode:
         * the smallest bucket >= target, else the largest smaller one, else
         * empty. (The rule the pre-STRIP-2 single-row subquery applied.)
         */
        static std::string bestFitPath(const std::vector<CachedThumbnailRef> &refs, int target);

        /**
         * @brief Ensure thumbnail_cache table and indices exist
         * @param db Database manager
         * @return true if successful, false otherwise
         */
        static bool ensureTable(DatabaseManager &db);

        /**
         * @brief THUMB-1: the on-disk filename for a cached thumbnail —
         * "<hash(source_path)>_<size>_r<revision>.jpg". The single place both
         * generation paths (pipeline helper, ThumbnailService) get it from.
         */
        static std::string cacheFileName(const std::string &source_path, int size,
                                         int render_revision = kNoRenderRevision);

        /**
         * @brief Get thumbnail record for a source path and size
         * @param db Database manager
         * @param source_path Source file path
         * @param size Thumbnail size (128, 256, 512, or 1024)
         * @return Optional record if found
         */
        static std::optional<ThumbnailCacheRecord> getThumbnail(DatabaseManager &db,
                                                                const std::string &source_path,
                                                                int size,
                                                                int render_revision = kNoRenderRevision);

        // lookup_failed (optional) is set true when the query itself failed
        // (e.g. DB session pool starved) — callers must treat that as "try
        // again", NOT as a cache miss, or they'd re-submit generation for an
        // asset that is already cached.
        // ADJ-1: render_revision selects the file's current revision (0 for an
        // un-edited file); a row of another revision is never a hit.
        static std::optional<ThumbnailCacheRecord> getBestFitThumbnail(DatabaseManager &db,
                                                                       const std::string &source_path,
                                                                       int min_size,
                                                                       bool *lookup_failed = nullptr,
                                                                       int render_revision = kNoRenderRevision);

        /**
         * @brief ADJ-1: every cached row of a source path, all sizes and
         * revisions, so an edit can unlink the files before deleting the rows.
         */
        static std::vector<ThumbnailCacheRecord> listForPath(DatabaseManager &db, const std::string &source_path);

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
                                     int64_t timestamp,
                                     int render_revision = kNoRenderRevision);

        /**
         * @brief Delete thumbnail cache entry
         * @param db Database manager
         * @param source_path Source file path
         * @param size Thumbnail size (if 0, deletes all sizes for this path)
         * @return true if successful, false otherwise
         */
        static bool deleteThumbnail(DatabaseManager &db,
                                    const std::string &source_path,
                                    int size = 0,
                                    int render_revision = kNoRenderRevision);

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
    
        /**
         * @brief MP-3: delete all size=0 rows (full-resolution renders that
         * leaked past validation and could never be served by the best-fit
         * query). Returns the cached paths so the caller can unlink the files.
         */
        static std::vector<std::string> purgeSizeZeroThumbnails(DatabaseManager &db);

        /**
         * @brief MP-2: cached-file paths in least-recently-used order (for
         * DiskCache eviction).
         */
        static std::vector<std::string> lruCachedPaths(DatabaseManager &db, int limit);

        /**
         * @brief MP-2: delete rows whose files were evicted from disk, so no
         * dangling rows survive (dangling rows caused regeneration storms).
         */
        static bool deleteByCachedPaths(DatabaseManager &db, const std::vector<std::string> &cached_paths);
};

} // namespace MediaDedup
