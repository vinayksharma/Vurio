#include "database/database_manager.hpp"
#include <Poco/Data/SQLite/Connector.h>
#include <Poco/Data/Session.h>
#include <Poco/Logger.h>
#include <Poco/Data/RecordSet.h>
#include "database/session_manager.hpp"
#include "database/user_settings_ops.hpp"
#include "config/unified_observable_config.hpp"
#include <fstream>
#include <sstream>
#include "database/sql_constants.hpp"

namespace MediaDedup
{

    DatabaseManager::DatabaseManager(const std::string &db_path, std::shared_ptr<UnifiedObservableConfigManager> config_manager)
        : db_path_(db_path), connected_(false), logger_(Poco::Logger::get("DatabaseManager")), config_manager_(config_manager) {}

    DatabaseManager::~DatabaseManager() = default;

    bool DatabaseManager::initialize()
    {
        try
        {
            // Get pool sizes from config, with defaults if config_manager is not available
            int pool_min = 4;
            int pool_max = 20;
            
            if (config_manager_)
            {
                pool_min = config_manager_->getPropertyValue<int>("database.session.poolMin", 4);
                pool_max = config_manager_->getPropertyValue<int>("database.session.poolMax", 20);
                logger_.information("Using database session pool: min=%d, max=%d", pool_min, pool_max);
            }
            else
            {
                logger_.warning("No config manager provided, using default session pool sizes: min=%d, max=%d", pool_min, pool_max);
            }
            
            // Initialize SessionManager and its pool
            session_manager_ = std::make_unique<SessionManager>("SQLite", db_path_, 
                                                               static_cast<std::size_t>(pool_min), 
                                                               static_cast<std::size_t>(pool_max), 
                                                               60);
            if (session_manager_->initialize())
            {
                connected_ = true;
                logger_.information("Database connected successfully: " + db_path_);
                
                // Subscribe to config changes for pool sizes
                if (config_manager_)
                {
                    config_manager_->subscribeToConfigChanges([this](const ConfigChangeEvent &event)
                                                             {
                        if (event.key == "database.session.poolMin" || event.key == "database.session.poolMax")
                        {
                            logger_.warning("Database session pool size changed ('%s'). Pool size changes require server restart to take effect.", event.key);
                        } });
                }
                
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
                // Get pool sizes from config, with defaults if config_manager is not available
                int pool_min = 4;
                int pool_max = 20;
                
                if (config_manager_)
                {
                    pool_min = config_manager_->getPropertyValue<int>("database.session.poolMin", 4);
                    pool_max = config_manager_->getPropertyValue<int>("database.session.poolMax", 20);
                }
                
                session_manager_ = std::make_unique<SessionManager>("SQLite", db_path_, 
                                                                   static_cast<std::size_t>(pool_min), 
                                                                   static_cast<std::size_t>(pool_max), 
                                                                   60);
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

    void DatabaseManager::setSessionAcquireTimeoutMs(int ms)
    {
        if (session_manager_)
            session_manager_->setAcquireTimeoutMs(ms);
    }

    void DatabaseManager::setSessionAcquireBackoffMs(int ms)
    {
        if (session_manager_)
            session_manager_->setBackoffMs(ms);
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
