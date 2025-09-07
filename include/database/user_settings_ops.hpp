#pragma once

#include <string>
#include <unordered_map>

namespace MediaDedup
{

    class DatabaseManager;

    class UserSettingsOps
    {
    public:
        static bool ensureTable(DatabaseManager &db_manager);
        static bool upsert(DatabaseManager &db_manager, const std::string &key, const std::string &value);
        static bool remove(DatabaseManager &db_manager, const std::string &key);
        static bool get(DatabaseManager &db_manager, const std::string &key, std::string &value_out);
        static std::unordered_map<std::string, std::string> list(DatabaseManager &db_manager);

        // Media locations management
        // Registers a directory path under a deterministic key and stores path->key mapping to allow overwrite-by-path
        static bool registerMediaLocation(DatabaseManager &db_manager, const std::string &directory_path);
        static bool deregisterMediaLocation(DatabaseManager &db_manager, const std::string &directory_path);
        static std::unordered_map<std::string, std::string> listMediaLocations(DatabaseManager &db_manager);
    };

} // namespace MediaDedup
