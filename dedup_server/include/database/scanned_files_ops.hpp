#pragma once

#include <string>
#include <vector>
#include <optional>
#include "config/unified_observable_config.hpp"

namespace Poco { namespace Data { class Session; } }

namespace MediaDedup
{
    class DatabaseManager;

    struct ScannedFileRow
    {
        int id = 0;
        std::string file_path;
        std::string relative_path;
        std::string share_name;
        std::string file_name;
        std::string file_metadata;
        int processed = 0; // 0=unprocessed, 1=in-progress, 2=completed, 3=ignored, <0=error codes
        std::optional<std::string> ignore_reason;
        std::string links;
        bool is_network_file = false;
        std::string location_key; // Full user_settings.key format (e.g., "mediaLocation:abc123")
        std::string created_at;
        std::optional<std::string> exif_datetime;
        std::optional<std::string> exif_camera;
        std::optional<std::string> exif_resolution;
        // DB-2: the raw exif_blob is no longer carried or written — it was
        // 2.83 GB of write-only data (half the database). The schema column
        // survives as NULL for old installs.
    };

    class ScannedFilesOps
    {
    public:
        static bool ensureTable(DatabaseManager &db);
        static bool upsert(DatabaseManager &db, const ScannedFileRow &row);
        static bool removeByPath(DatabaseManager &db, const std::string &file_path);
        static bool removeByLocationKey(DatabaseManager &db, const std::string &location_key);
        
        /**
         * @brief Completely removes a file from the database, including all its associated
         * metadata (thumbnails, artifacts, errors, duplicates, ejected history).
         * 
         * Note: This does NOT automatically promote a duplicate group representative if the file
         * is one. The caller must handle representative promotion prior to calling this.
         */
        static bool completelyRemoveById(DatabaseManager &db, int file_id);
        static std::optional<ScannedFileRow> getByPath(DatabaseManager &db, const std::string &file_path);
        static std::vector<ScannedFileRow> listAll(DatabaseManager &db);
        static int count(DatabaseManager &db);
        static int countProcessed(DatabaseManager &db);
        static int countProcessedSuccess(DatabaseManager &db);
        static int countProcessedUpTo(DatabaseManager &db, int max_id);
        static int countError(DatabaseManager &db);
        static int countQueued(DatabaseManager &db);

        // Unfiltered count methods (for backward compatibility and testing)
        static int countAll(DatabaseManager &db);
        static int countProcessedAll(DatabaseManager &db);
        static int countErrorAll(DatabaseManager &db);
        static int countQueuedAll(DatabaseManager &db);

        // Helper methods for location_key filtering
        static std::string getLocationKey(DatabaseManager &db, const std::string &file_path);

        // Processing state management
        static bool markProcessed(DatabaseManager &db, const std::string &file_path, int state);
        static bool markIgnored(DatabaseManager &db, const std::string &file_path, const std::string &reason);
        static bool markProcessedWithEscalation(DatabaseManager &db, const std::string &file_path, int state);
        static bool setLinks(DatabaseManager &db, const std::string &file_path, const std::vector<int> &link_ids);
        static std::vector<int> getLinks(DatabaseManager &db, const std::string &file_path);
        static std::vector<ScannedFileRow> listUnprocessed(DatabaseManager &db, int limit = -1);
        static std::vector<ScannedFileRow> listUnprocessedAll(DatabaseManager &db, int limit = -1);
        static std::vector<ScannedFileRow> listErroredFiles(DatabaseManager &db);
        static std::vector<ScannedFileRow> listIgnoredFiles(DatabaseManager &db);

        // Reset all errors to unprocessed (0)
        static int resetAllErrors(DatabaseManager &db);

        // Update metadata for a file
        static bool updateMetadata(DatabaseManager &db, const std::string &file_path, const std::string &file_metadata);
        
        /**
         * @brief DB-7: all GetServerStatus scanned_files counts in ONE query.
         *
         * Predicates mirror count()/countProcessed()/countError()/
         * countProcessedSuccess()/countProcessedUpTo() — previously the status
         * poll (every 2 s) took five separate pool leases for five COUNTs.
         */
        struct ScanStatusCounts
        {
            int total = 0;
            int processed = 0;        // processed = 2 or 3
            int errors = 0;           // processed < 0, excluding -2 / -99
            int processed_success = 0; // processed = 2
            int processed_up_to = 0;   // processed = 2 AND id <= max_id
        };
        static bool getStatusCounts(DatabaseManager &db, int max_id, ScanStatusCounts &out);

        /**
         * @brief DB-7: upsert many rows in ONE transaction (one WAL commit).
         *
         * The scanner previously issued one autocommit upsert per file — a
         * 160k-file rescan was 160k pool checkouts and WAL commits. Returns
         * the number of rows written (0 on failure; all-or-nothing).
         */
        static int upsertBatch(DatabaseManager &db, const std::vector<ScannedFileRow> &rows);

        // Update EXIF metadata (parsed fields only; DB-2 removed the raw blob)
        static bool updateExifMetadata(DatabaseManager &db, const std::string &file_path,
                                     const std::string &exif_datetime, const std::string &exif_camera,
                                     const std::string &exif_resolution);

        // Clear processing flags
        static int clearProcessingFlags(DatabaseManager &db);

        // Efficient file existence check (optimized for FilesManager scans)
        // Returns true if file_path exists in scanned_files table
        static bool fileExists(DatabaseManager &db, const std::string &file_path);

        // Path change operations
        // Get count of files for a location_key
        static int countFilesByLocationKey(
            DatabaseManager &db,
            const std::string &location_key
        );

        // Get random sample of files for a location_key (for verification)
        static std::vector<ScannedFileRow> getRandomFilesByLocationKey(
            DatabaseManager &db,
            const std::string &location_key,
            int limit
        );

        // Get files in batches for a location_key (for batch updates)
        static std::vector<ScannedFileRow> getFilesByLocationKeyBatch(
            DatabaseManager &db,
            const std::string &location_key,
            int limit,
            int last_id = 0
        );

        // Get all files for a location_key (kept for backward compatibility, but should use batch method for large datasets)
        static std::vector<ScannedFileRow> getFilesByLocationKey(
            DatabaseManager &db,
            const std::string &location_key
        );

        // Update file path in scanned_files table
        static bool updateFilePath(
            DatabaseManager &db,
            int file_id,
            const std::string &new_path,
            const std::string &new_relative_path,
            const std::string &new_location_key
        );
        static bool updateFilePath(
            Poco::Data::Session &sess,
            int file_id,
            const std::string &new_path,
            const std::string &new_relative_path,
            const std::string &new_location_key
        );

        static int updateFilePathInImageArtifacts(
            DatabaseManager &db,
            const std::string &old_path,
            const std::string &new_path
        );
        static int updateFilePathInImageArtifacts(
            Poco::Data::Session &sess,
            const std::string &old_path,
            const std::string &new_path
        );

        static int updateFilePathInProcessingErrors(
            DatabaseManager &db,
            const std::string &old_path,
            const std::string &new_path
        );
        static int updateFilePathInProcessingErrors(
            Poco::Data::Session &sess,
            const std::string &old_path,
            const std::string &new_path
        );

        static int updateFilePathInDuplicateGroups(
            DatabaseManager &db,
            const std::string &old_path,
            const std::string &new_path
        );
        static int updateFilePathInDuplicateGroups(
            Poco::Data::Session &sess,
            const std::string &old_path,
            const std::string &new_path
        );

        static int updateFilePathInDuplicateMembers(
            DatabaseManager &db,
            const std::string &old_path,
            const std::string &new_path
        );
        static int updateFilePathInDuplicateMembers(
            Poco::Data::Session &sess,
            const std::string &old_path,
            const std::string &new_path
        );

        static int updateFilePathInThumbnailCache(
            DatabaseManager &db,
            const std::string &old_path,
            const std::string &new_path
        );
        static int updateFilePathInThumbnailCache(
            Poco::Data::Session &sess,
            const std::string &old_path,
            const std::string &new_path
        );

        static int updateFilePathInPreviewCache(
            DatabaseManager &db,
            const std::string &old_path,
            const std::string &new_path
        );
        static int updateFilePathInPreviewCache(
            Poco::Data::Session &sess,
            const std::string &old_path,
            const std::string &new_path
        );

    private:
        // Helper function to execute filtered count queries
    };
}
