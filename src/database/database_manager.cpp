#include "database/database_manager.hpp"
#include <Poco/Data/SQLite/Connector.h>
#include <Poco/Data/Session.h>
#include <Poco/Logger.h>
#include <Poco/Data/RecordSet.h>
#include "database/session_manager.hpp"
#include "database/user_settings_ops.hpp"
#include <fstream>
#include <sstream>
#include "database/sql_constants.hpp"

namespace MediaDedup
{

    DatabaseManager::DatabaseManager(const std::string &db_path)
        : db_path_(db_path), connected_(false), logger_(Poco::Logger::get("DatabaseManager")) {}

    DatabaseManager::~DatabaseManager() = default;

    bool DatabaseManager::initialize()
    {
        try
        {
            // Initialize SessionManager and its pool
            session_manager_ = std::make_unique<SessionManager>("SQLite", db_path_, 1, 8, 60);
            if (session_manager_->initialize())
            {
                connected_ = true;
                logger_.information("Database connected successfully: " + db_path_);
                return true;
            }
            else
            {
                logger_.error("Failed to connect to database: " + db_path_);
                return false;
            }
        }
        catch (const std::exception &e)
        {
            logger_.error("Database initialization failed: " + std::string(e.what()));
            return false;
        }
    }

    bool DatabaseManager::isConnected() const
    {
        return connected_;
    }

    // Removed direct getSession()/getConnectedSession(); use acquireSessionLease()

    bool DatabaseManager::ensureSessionManagerReady()
    {
        if (!session_manager_)
        {
            try
            {
                session_manager_ = std::make_unique<SessionManager>("SQLite", db_path_, 1, 8, 60);
                if (!session_manager_->initialize())
                    return false;
            }
            catch (const std::exception &e)
            {
                logDatabaseError("ensureSessionManagerReady", e.what());
                return false;
            }
        }
        return true;
    }

    SessionManager::Lease DatabaseManager::acquireSessionLease()
    {
        if (!ensureSessionManagerReady())
        {
            throw std::runtime_error("Database session manager not initialized");
        }
        return session_manager_->acquireLease();
    }

    // releaseLease removed (managed by SessionManager)

    bool DatabaseManager::backupDatabase(const std::string &backup_path)
    {
        // Not implemented
        return false;
    }

    bool DatabaseManager::executeSQL(const std::string &sql)
    {
        try
        {
            auto lease = acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            Poco::Data::Statement stmt(sess);
            stmt << sql, Poco::Data::Keywords::now;
            return true;
        }
        catch (const std::exception &e)
        {
            logDatabaseError("executeSQL", e.what());
            return false;
        }
    }

    void DatabaseManager::logDatabaseError(const std::string &operation, const std::string &error)
    {
        logger_.error("Database error during " + operation + ": " + error);
    }

    bool DatabaseManager::tableExists(const std::string &table_name)
    {
        try
        {
            auto lease = acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            int count = 0;
            Poco::Data::Statement stmt(sess);
            std::string tableNameLvalue = table_name;
            stmt << std::string(MediaDedup::SQL::kTableExistsQuery),
                Poco::Data::Keywords::use(tableNameLvalue),
                Poco::Data::Keywords::into(count),
                Poco::Data::Keywords::now;
            return count > 0;
        }
        catch (const std::exception &e)
        {
            logDatabaseError("tableExists", e.what());
            return false;
        }
    }

    bool DatabaseManager::ensureTableExists(const std::string &table_name, std::string_view create_if_not_exists_sql)
    {
        if (tableExists(table_name))
        {
            return true;
        }
        return executeSQL(std::string(create_if_not_exists_sql));
    }

    std::string DatabaseManager::readTextFile(const std::string &path)
    {
        try
        {
            std::ifstream in(path);
            if (!in)
            {
                return {};
            }
            std::ostringstream ss;
            ss << in.rdbuf();
            return ss.str();
        }
        catch (...)
        {
            return {};
        }
    }

    // User settings operations moved to user_settings_* files

} // namespace MediaDedup
