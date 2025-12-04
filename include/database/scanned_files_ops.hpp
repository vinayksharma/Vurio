#pragma once

#include <string>
#include <vector>
#include <optional>
#include "config/unified_observable_config.hpp"

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
        int processed = 0; // 0=unprocessed, 1=in-progress, 2=completed, <0=error codes
        std::string links;
        bool is_network_file = false;
        std::string location_key; // Full user_settings.key format (e.g., "mediaLocation:abc123")
        std::string created_at;
    };

    class ScannedFilesOps
    {
    public:
        static bool ensureTable(DatabaseManager &db);
        static bool upsert(DatabaseManager &db, const ScannedFileRow &row);
        static bool removeByPath(DatabaseManager &db, const std::string &file_path);
        static std::optional<ScannedFileRow> getByPath(DatabaseManager &db, const std::string &file_path);
        static std::vector<ScannedFileRow> listAll(DatabaseManager &db);
        static int count(DatabaseManager &db);
        static int countProcessed(DatabaseManager &db);
        static int countError(DatabaseManager &db);
        static int countQueued(DatabaseManager &db);

        // Unfiltered count methods (for backward compatibility and testing)
        static int countAll(DatabaseManager &db);
        static int countProcessedAll(DatabaseManager &db);
        static int countErrorAll(DatabaseManager &db);
        static int countQueuedAll(DatabaseManager &db);

        // Helper methods for location_key filtering
        static std::vector<std::string> getRegisteredLocationKeys(DatabaseManager &db);
        static std::string getLocationKey(DatabaseManager &db, const std::string &file_path);

        // Processing state management
        static bool markProcessed(DatabaseManager &db, const std::string &file_path, int state);
        static bool markProcessedWithEscalation(DatabaseManager &db, const std::string &file_path, int state);
        static bool setLinks(DatabaseManager &db, const std::string &file_path, const std::vector<int> &link_ids);
        static std::vector<int> getLinks(DatabaseManager &db, const std::string &file_path);
        static std::vector<ScannedFileRow> listUnprocessed(DatabaseManager &db, int limit = -1);
        static std::vector<ScannedFileRow> listUnprocessedAll(DatabaseManager &db, int limit = -1);

        // Reset all errors to unprocessed (0)
        static int resetAllErrors(DatabaseManager &db);

        // Update metadata for a file
        static bool updateMetadata(DatabaseManager &db, const std::string &file_path, const std::string &file_metadata);

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
            int offset
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

        // Update file path in other tables
        static int updateFilePathInImageArtifacts(
            DatabaseManager &db,
            const std::string &old_path,
            const std::string &new_path
        );

        static int updateFilePathInProcessingErrors(
            DatabaseManager &db,
            const std::string &old_path,
            const std::string &new_path
        );

        static int updateFilePathInDuplicateGroups(
            DatabaseManager &db,
            const std::string &old_path,
            const std::string &new_path
        );

        static int updateFilePathInDuplicateMembers(
            DatabaseManager &db,
            const std::string &old_path,
            const std::string &new_path
        );

        static int updateFilePathInThumbnailCache(
            DatabaseManager &db,
            const std::string &old_path,
            const std::string &new_path
        );

    private:
        // Helper function to execute filtered count queries
        static int executeFilteredCount(DatabaseManager &db, const std::string &base_query);
    };
}
