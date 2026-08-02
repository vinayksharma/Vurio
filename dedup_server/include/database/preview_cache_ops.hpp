#pragma once

#include <string>
#include <optional>
#include <cstdint>

namespace MediaDedup
{
    class DatabaseManager;

    struct PreviewCacheRecord
    {
        int id{0};
        std::string source_path;
        std::string cached_path;
        int64_t file_size_bytes{0};
        int64_t source_modified_at{0};
        int64_t created_at{0};
        int64_t last_accessed_at{0};
    };

    class PreviewCacheOps
    {
    public:
        /**
         * @brief Ensure the preview_cache table exists
         */
        static bool ensureTable(DatabaseManager &db);

        /**
         * @brief Get a preview by source path
         */
        // lookup_failed (optional) is set true when the query itself failed
        // (e.g. DB session pool starved) — callers must treat that as "try
        // again", NOT as a cache miss.
        static std::optional<PreviewCacheRecord> getPreview(DatabaseManager &db, const std::string &source_path,
                                                            bool *lookup_failed = nullptr);

        /**
         * @brief Insert or update a preview record
         */
        static bool upsertPreview(DatabaseManager &db, const PreviewCacheRecord &record);

        /**
         * @brief Build a fully populated record for a freshly generated preview.
         *
         * Fills source_modified_at (source file mtime), file_size_bytes (of the
         * cached file), and created_at/last_accessed_at (now). QW-8
         * (docs/backend_optimization_plan.md): every generation site MUST use
         * this — a record written with source_modified_at = 0 permanently fails
         * ingest's freshness check and the RAW is re-transcoded although a good
         * preview exists.
         */
        static PreviewCacheRecord makeRecord(const std::string &source_path,
                                             const std::string &cached_path);

        /**
         * @brief Update the last_accessed_at timestamp for a preview
         */
        static bool updateAccessTime(DatabaseManager &db, const std::string &source_path, int64_t timestamp);

        /**
         * @brief Delete a preview record
         */
        static bool deletePreview(DatabaseManager &db, const std::string &source_path);

        /**
         * @brief Delete entries whose source_modified_at is older than the given timestamp
         * @return Number of deleted rows
         */
        static int deleteStaleEntries(DatabaseManager &db, int64_t before_timestamp);

        /**
         * @brief Get total number of cached previews
         */
        static int getCount(DatabaseManager &db);

        /**
         * @brief Clears all entries from the preview cache
         * @param db Database manager
         * @return true if successful, false otherwise
         */
        static bool clearCache(DatabaseManager &db);

        /**
         * @brief Update the base path for all cached previews (used when moving cache location)
         * @param db Database manager
         * @param old_base_path Old absolute base path
         * @param new_base_path New absolute base path
         * @return true if successful, false otherwise
         */
        static bool updateCacheLocation(DatabaseManager &db, const std::string &old_base_path, const std::string &new_base_path);
    };

} // namespace MediaDedup
