#pragma once

#include <string>
#include <unordered_map>
#include <mutex>
#include <Poco/Logger.h>

#include "database/database_manager.hpp"

namespace MediaDedup
{

    class UserSettingsService
    {
    public:
        explicit UserSettingsService(DatabaseManager &db_manager)
            : db_manager_(db_manager), logger_(Poco::Logger::get("UserSettingsService")) {}

        bool initialize()
        {
            return db_manager_.ensureUserSettingsTable();
        }

        bool upsertSetting(const std::string &key, const std::string &value)
        {
            return db_manager_.userSettingsUpsert(key, value);
        }

        bool deleteSetting(const std::string &key)
        {
            return db_manager_.userSettingsDelete(key);
        }

        bool getSetting(const std::string &key, std::string &value_out)
        {
            return db_manager_.userSettingsGet(key, value_out);
        }

        std::unordered_map<std::string, std::string> listSettings()
        {
            return db_manager_.userSettingsList();
        }

    private:
        DatabaseManager &db_manager_;
        Poco::Logger &logger_;
    };

} // namespace MediaDedup
