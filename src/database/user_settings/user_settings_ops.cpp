#include "database/user_settings_ops.hpp"
#include "database/database_manager.hpp"
#include "database/sql_constants.hpp"
#include <Poco/Data/Session.h>
#include <Poco/Data/Statement.h>
#include <Poco/Data/RecordSet.h>

namespace MediaDedup
{

    bool UserSettingsOps::ensureTable(DatabaseManager &db_manager)
    {
        return db_manager.ensureTableExists("user_settings", SQL::kCreateUserSettingsTable);
    }

    bool UserSettingsOps::upsert(DatabaseManager &db_manager, const std::string &key, const std::string &value)
    {
        try
        {
            auto lease = db_manager.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            Poco::Data::Statement stmt(sess);
            std::string keyCopy = key, valueCopy = value;
            stmt << std::string(SQL::kUpsertUserSetting),
                Poco::Data::Keywords::use(keyCopy),
                Poco::Data::Keywords::use(valueCopy),
                Poco::Data::Keywords::now;
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool UserSettingsOps::remove(DatabaseManager &db_manager, const std::string &key)
    {
        try
        {
            auto lease = db_manager.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            Poco::Data::Statement stmt(sess);
            std::string keyCopy = key;
            stmt << std::string(SQL::kDeleteUserSetting),
                Poco::Data::Keywords::use(keyCopy),
                Poco::Data::Keywords::now;
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool UserSettingsOps::get(DatabaseManager &db_manager, const std::string &key, std::string &value_out)
    {
        try
        {
            auto lease = db_manager.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            Poco::Data::Statement stmt(sess);
            std::string keyCopy = key;
            stmt << std::string(SQL::kSelectUserSetting),
                Poco::Data::Keywords::use(keyCopy),
                Poco::Data::Keywords::now;
            Poco::Data::RecordSet rs(stmt);
            if (rs.moveFirst())
            {
                value_out = rs[0].convert<std::string>();
                return true;
            }
            return false;
        }
        catch (...)
        {
            return false;
        }
    }

    std::unordered_map<std::string, std::string> UserSettingsOps::list(DatabaseManager &db_manager)
    {
        std::unordered_map<std::string, std::string> result;
        try
        {
            auto lease = db_manager.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            Poco::Data::Statement stmt(sess);
            stmt << std::string(SQL::kListUserSettings), Poco::Data::Keywords::now;
            Poco::Data::RecordSet rs(stmt);
            bool more = rs.moveFirst();
            while (more)
            {
                std::string key = rs[0].convert<std::string>();
                std::string value = rs[1].convert<std::string>();
                result.emplace(std::move(key), std::move(value));
                more = rs.moveNext();
            }
        }
        catch (...)
        {
        }
        return result;
    }

} // namespace MediaDedup
