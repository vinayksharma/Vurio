#include "database/database_manager.hpp"
#include <Poco/Data/SQLite/Connector.h>
#include <Poco/Data/Session.h>
#include <Poco/Logger.h>
#include <Poco/Data/RecordSet.h>
#include "database/session_manager.hpp"
#include "database/user_settings_ops.hpp"
#include "database/vector_index.hpp"
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
            int pool_max = 32;
            int cache_pages = 2000;
            std::string temp_store = "memory";

            if (config_manager_)
            {
                pool_min = config_manager_->getPropertyValue<int>("database.session.poolMin", 4);
                pool_max = config_manager_->getPropertyValue<int>("database.session.poolMax", 32);
                // QW-7: these two knobs existed in config but were read
                // nowhere; they now drive the per-connection pragmas.
                cache_pages = config_manager_->getPropertyValue<int>("database.sqlite.cacheSize", 2000);
                temp_store = config_manager_->getPropertyValue<std::string>("database.sqlite.tempStore", "memory");
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
                                                               60, cache_pages, temp_store);
            if (session_manager_->initialize())
            {
                connected_ = true;
                logger_.information("Database connected successfully: " + db_path_);

                // DB-1/DB-6: give the query planner statistics. Without
                // sqlite_stat1 every plan is chosen from defaults (measured:
                // the day-view suppression subquery planned as a full
                // duplicate_members scan). One-time full ANALYZE on a
                // stats-less database (~0.5 s on a 5.6 GB library); cheap
                // `PRAGMA optimize` keeps stats fresh on later startups.
                try
                {
                    auto lease = session_manager_->acquireLease();
                    Poco::Data::Session &sess = lease.get();
                    int has_stats = 0;
                    Poco::Data::Statement check(sess);
                    check << "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='sqlite_stat1'",
                        Poco::Data::Keywords::into(has_stats), Poco::Data::Keywords::now;
                    Poco::Data::Statement stmt(sess);
                    if (has_stats == 0)
                    {
                        stmt << "ANALYZE", Poco::Data::Keywords::now;
                        logger_.information("Ran one-time ANALYZE to seed query-planner statistics");
                    }
                    else
                    {
                        stmt << "PRAGMA optimize", Poco::Data::Keywords::now;
                    }
                }
                catch (const std::exception &e)
                {
                    logger_.warning("ANALYZE/optimize skipped: " + std::string(e.what()));
                }

                // Optional vector index (sqlite-vec) for KNN duplicate search
                if (config_manager_)
                {
                    bool vec_enabled = config_manager_->getPropertyValue<bool>("database.vector.enabled", false);
                    std::string vec_path = config_manager_->getPropertyValue<std::string>("database.vector.extensionPath", "");
                    int embedding_dim = config_manager_->getPropertyValue<int>("media.image.embeddingDim", 512);
                    if (vec_enabled && !vec_path.empty() && embedding_dim > 0)
                    {
                        vector_index_ = std::make_unique<VectorIndex>();
                        if (vector_index_->init(db_path_, vec_path, embedding_dim))
                            logger_.information("Vector index enabled: extension=%s, dim=%d", vec_path.c_str(), embedding_dim);
                        else
                        {
                            logger_.warning("Vector index init failed; duplicate finder will use linear scan.");
                            vector_index_.reset();
                        }
                    }
                }

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

    std::string DatabaseManager::getDatabasePath() const
    {
        return db_path_;
    }

    VectorIndex *DatabaseManager::getVectorIndex()
    {
        return vector_index_.get();
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
                int pool_max = 32;
                int cache_pages = 2000;
                std::string temp_store = "memory";

                if (config_manager_)
                {
                    pool_min = config_manager_->getPropertyValue<int>("database.session.poolMin", 4);
                    pool_max = config_manager_->getPropertyValue<int>("database.session.poolMax", 32);
                    cache_pages = config_manager_->getPropertyValue<int>("database.sqlite.cacheSize", 2000);
                    temp_store = config_manager_->getPropertyValue<std::string>("database.sqlite.tempStore", "memory");
                }

                session_manager_ = std::make_unique<SessionManager>("SQLite", db_path_,
                                                                   static_cast<std::size_t>(pool_min),
                                                                   static_cast<std::size_t>(pool_max),
                                                                   60, cache_pages, temp_store);
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
        try
        {
            auto lease = acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            
            // VACUUM INTO safely creates a transactionally consistent copy of the database
            // even if it is currently in WAL mode. It does not block concurrent writers.
            std::string path_copy = backup_path;
            Poco::Data::Statement stmt(sess);
            stmt << "VACUUM INTO ?", Poco::Data::Keywords::use(path_copy), Poco::Data::Keywords::now;
            
            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("DatabaseManager").error("Failed to backup database to %s: %s", backup_path, std::string(e.what()));
            return false;
        }
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
