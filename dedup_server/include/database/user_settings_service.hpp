#pragma once

#include <string>
#include <unordered_map>
#include <mutex>
#include <Poco/Logger.h>

#include "database/database_manager.hpp"
#include "database/user_settings_ops.hpp"

namespace MediaDedup
{

    class UserSettingsService
    {
    public:
        explicit UserSettingsService(DatabaseManager &db_manager)
            : db_manager_(db_manager), logger_(Poco::Logger::get("UserSettingsService")) {}

        bool initialize() { return UserSettingsOps::ensureTable(db_manager_); }

        bool upsertSetting(const std::string &key, const std::string &value) { return UserSettingsOps::upsert(db_manager_, key, value); }

        bool deleteSetting(const std::string &key) { return UserSettingsOps::remove(db_manager_, key); }

        bool getSetting(const std::string &key, std::string &value_out) { return UserSettingsOps::get(db_manager_, key, value_out); }

        std::unordered_map<std::string, std::string> listSettings() { return UserSettingsOps::list(db_manager_); }

        // Media location helpers moved to FilesService

    private:
        DatabaseManager &db_manager_;
        Poco::Logger &logger_;
    };

} // namespace MediaDedup
