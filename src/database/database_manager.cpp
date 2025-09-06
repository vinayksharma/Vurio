#include "database/database_manager.hpp"
#include <Poco/Data/SQLite/Connector.h>
#include <Poco/Data/Session.h>
#include <Poco/Logger.h>
#include <Poco/Data/RecordSet.h>
#include "database/session_manager.hpp"
#include <fstream>
#include <sstream>

namespace MediaDedup
{

    DatabaseManager::DatabaseManager(const std::string &db_path)
        : db_path_(db_path), connected_(false), logger_(Poco::Logger::get("DatabaseManager"))
    {
        // TODO: Implement database initialization
        // Initialize session with a dummy connection string for now
        // This will be properly initialized in the initialize() method
    }

    DatabaseManager::~DatabaseManager()
    {
        // TODO: Implement cleanup
    }

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

    void DatabaseManager::close()
    {
        // TODO: Implement database connection closing
    }

    bool DatabaseManager::createTables()
    {
        // TODO: Implement table creation
        return false;
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

    bool DatabaseManager::storeMediaFile(const std::string &file_path,
                                         const std::string &file_hash,
                                         uint64_t file_size,
                                         const std::string &file_type,
                                         const std::string &metadata)
    {
        // TODO: Implement media file storage
        return false;
    }

    std::vector<std::string> DatabaseManager::getMediaFilesByHash(const std::string &file_hash)
    {
        // TODO: Implement hash-based file retrieval
        return {};
    }

    std::string DatabaseManager::getMediaFileHash(const std::string &file_path)
    {
        // TODO: Implement path-based hash retrieval
        return "";
    }

    bool DatabaseManager::updateMediaFileMetadata(const std::string &file_path, const std::string &metadata)
    {
        // TODO: Implement metadata update
        return false;
    }

    bool DatabaseManager::deleteMediaFile(const std::string &file_path)
    {
        // TODO: Implement file deletion
        return false;
    }

    std::vector<std::string> DatabaseManager::findDuplicatesByHash(const std::string &file_hash)
    {
        // TODO: Implement duplicate detection
        return {};
    }

    std::unordered_map<std::string, std::vector<std::string>> DatabaseManager::findAllDuplicates()
    {
        // TODO: Implement bulk duplicate detection
        return {};
    }

    std::unordered_map<std::string, size_t> DatabaseManager::getDuplicateStatistics()
    {
        // TODO: Implement statistics generation
        return {};
    }

    bool DatabaseManager::vacuumDatabase()
    {
        // TODO: Implement database optimization
        return false;
    }

    std::unordered_map<std::string, std::string> DatabaseManager::getDatabaseStats()
    {
        // TODO: Implement database statistics
        return {};
    }

    bool DatabaseManager::backupDatabase(const std::string &backup_path)
    {
        // TODO: Implement database backup
        return false;
    }

    bool DatabaseManager::createMediaFilesTable()
    {
        // TODO: Implement media files table creation
        return false;
    }

    bool DatabaseManager::createFileHashesTable()
    {
        // TODO: Implement file hashes table creation
        return false;
    }

    bool DatabaseManager::createMetadataTable()
    {
        // TODO: Implement metadata table creation
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
            stmt << "SELECT COUNT(1) FROM sqlite_master WHERE type='table' AND name=?",
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

    bool DatabaseManager::ensureUserSettingsTable()
    {
        if (tableExists("user_settings"))
        {
            return true;
        }

        const std::string script_path = "src/database/dbscripts/create_user_settings.sql";
        std::string sql = readTextFile(script_path);
        if (sql.empty())
        {
            // Fallback: inline SQL
            sql = "CREATE TABLE IF NOT EXISTS user_settings (key TEXT PRIMARY KEY, value TEXT NOT NULL);";
        }
        return executeSQL(sql);
    }

    bool DatabaseManager::userSettingsUpsert(const std::string &key, const std::string &value)
    {
        try
        {
            auto lease = acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            Poco::Data::Statement stmt(sess);
            std::string keyCopy = key;
            std::string valueCopy = value;
            stmt << "INSERT INTO user_settings(key, value) VALUES(?, ?) ON CONFLICT(key) DO UPDATE SET value=excluded.value",
                Poco::Data::Keywords::use(keyCopy),
                Poco::Data::Keywords::use(valueCopy),
                Poco::Data::Keywords::now;
            return true;
        }
        catch (const std::exception &e)
        {
            logDatabaseError("userSettingsUpsert", e.what());
            return false;
        }
    }

    bool DatabaseManager::userSettingsDelete(const std::string &key)
    {
        try
        {
            auto lease = acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            Poco::Data::Statement stmt(sess);
            std::string keyCopy = key;
            stmt << "DELETE FROM user_settings WHERE key=?",
                Poco::Data::Keywords::use(keyCopy),
                Poco::Data::Keywords::now;
            return true;
        }
        catch (const std::exception &e)
        {
            logDatabaseError("userSettingsDelete", e.what());
            return false;
        }
    }

    bool DatabaseManager::userSettingsGet(const std::string &key, std::string &value_out)
    {
        try
        {
            auto lease = acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            Poco::Data::Statement stmt(sess);
            std::string keyCopy = key;
            stmt << "SELECT value FROM user_settings WHERE key=?",
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
        catch (const std::exception &e)
        {
            logDatabaseError("userSettingsGet", e.what());
            return false;
        }
    }

    std::unordered_map<std::string, std::string> DatabaseManager::userSettingsList()
    {
        std::unordered_map<std::string, std::string> result;
        try
        {
            auto lease = acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            Poco::Data::Statement stmt(sess);
            stmt << "SELECT key, value FROM user_settings",
                Poco::Data::Keywords::now;
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
        catch (const std::exception &e)
        {
            logDatabaseError("userSettingsList", e.what());
        }
        return result;
    }

} // namespace MediaDedup
