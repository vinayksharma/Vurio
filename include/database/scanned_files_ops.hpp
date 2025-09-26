#pragma once

#include <string>
#include <vector>
#include <optional>
#include "config/config_enums.hpp"
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
        int processed_fast = 0; // 0=unprocessed, >0=processed/state code
        int processed_balanced = 0;
        int processed_quality = 0;
        std::string links_fast;
        std::string links_balanced;
        std::string links_quality;
        bool is_network_file = false;
        std::string created_at;
    };

    // Deprecated local enum; use config::ServerMode for server-wide mode

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
        static int countProcessed(DatabaseManager &db, ServerMode mode);

        // Adjusted APIs based on stored data
        static bool markProcessed(DatabaseManager &db, const std::string &file_path, ServerMode mode, int state);
        static bool setLinks(DatabaseManager &db, const std::string &file_path, ServerMode mode, const std::vector<int> &link_ids);
        static std::vector<int> getLinks(DatabaseManager &db, const std::string &file_path, ServerMode mode);
        static std::vector<ScannedFileRow> listUnprocessed(DatabaseManager &db, ServerMode mode, int limit = -1);

        // Overloads that infer ServerMode from configuration
        static bool markProcessed(DatabaseManager &db, UnifiedObservableConfigManager &cfg,
                                  const std::string &file_path, int state);
        static bool setLinks(DatabaseManager &db, UnifiedObservableConfigManager &cfg,
                             const std::string &file_path, const std::vector<int> &link_ids);
        static std::vector<int> getLinks(DatabaseManager &db, UnifiedObservableConfigManager &cfg,
                                         const std::string &file_path);
        static std::vector<ScannedFileRow> listUnprocessed(DatabaseManager &db, UnifiedObservableConfigManager &cfg,
                                                           int limit = -1);
        static bool updateMetadata(DatabaseManager &db, const std::string &file_path, const std::string &file_metadata);

        // Clear processing flags
        static int clearProcessingFlags(DatabaseManager &db);
    };
}
