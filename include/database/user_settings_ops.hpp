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

        // Media locations are now handled by FilesService; keep no-ops for backward compatibility if needed
    };

} // namespace MediaDedup
