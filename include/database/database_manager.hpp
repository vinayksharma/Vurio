#pragma once

#include <Poco/Data/Session.h>
#include <Poco/Data/SQLite/Connector.h>
#include <Poco/Data/Statement.h>
#include <Poco/Data/RecordSet.h>
#include <Poco/Data/Row.h>
#include <Poco/Data/Column.h>
#include <Poco/Data/DataException.h>
#include <Poco/Logger.h>
#include <string>
#include <string_view>
#include <memory>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include "database/session_manager.hpp"

namespace MediaDedup
{
    // Forward declarations
    class UnifiedObservableConfigManager;
    class SessionManager;

    /**
     * @brief Database manager for the media deduplication server
     *
     * This class handles all database operations using Poco Data and SQLite:
     * - Database connection management
     * - Table creation and schema management
     * - CRUD operations for media files
     * - Hash storage and retrieval
     * - Duplicate detection queries
     */
    class DatabaseManager
    {
    public:
        /**
         * @brief Ensure a table exists, creating it with provided SQL if missing
         * @param table_name Logical table name to check in sqlite_master
         * @param create_if_not_exists_sql CREATE TABLE IF NOT EXISTS ... statement
         */
        bool ensureTableExists(const std::string &table_name, std::string_view create_if_not_exists_sql);
        // Session access via SessionManager
        SessionManager::Lease acquireSessionLease();
        /**
         * @brief Constructor
         * @param db_path Path to SQLite database file
         * @param config_manager Optional config manager for reactive pool sizing
         */
        explicit DatabaseManager(const std::string &db_path = "dedup_server.db", 
                                std::shared_ptr<UnifiedObservableConfigManager> config_manager = nullptr);

        /**
         * @brief Destructor
         */
        ~DatabaseManager();

        /**
         * @brief Initialize database connection
         * @return true if successful, false otherwise
         */
        bool initialize();

        // Removed unimplemented close/createTables

        /**
         * @brief Check if database is connected
         * @return true if connected, false otherwise
         */
        bool isConnected() const;

        /**
         * @brief Get database session
         * @return Reference to database session
         */
        // Removed direct Session accessors; use acquireSessionLease() instead

        // ...

        // User Settings operations are declared under user_settings_* files

        // Media file operations
        // Removed unimplemented media file operations

        /**
         * @brief Get media file by hash
         * @param file_hash File hash to search for
         * @return Vector of file paths with matching hash
         */
        // removed

        /**
         * @brief Get media file by path
         * @param file_path File path to search for
         * @return File hash if found, empty string otherwise
         */
        // removed

        /**
         * @brief Update media file metadata
         * @param file_path File path
         * @param metadata New metadata
         * @return true if successful, false otherwise
         */
        // removed

        /**
         * @brief Delete media file record
         * @param file_path File path to delete
         * @return true if successful, false otherwise
         */
        // removed

        // Duplicate detection operations
        /**
         * @brief Find duplicate files by hash
         * @param file_hash File hash to search for
         * @return Vector of duplicate file paths
         */
        // removed

        /**
         * @brief Find all duplicate files
         * @return Map of hash to vector of file paths
         */
        // removed

        /**
         * @brief Get duplicate statistics
         * @return Map of hash to duplicate count
         */
        // removed

        // Database maintenance
        /**
         * @brief Vacuum database (reclaim space)
         * @return true if successful, false otherwise
         */
        // removed

        /**
         * @brief Get database statistics
         * @return Map of statistics
         */
        // removed

        /**
         * @brief Backup database
         * @param backup_path Path for backup file
         * @return true if successful, false otherwise
         */
        bool backupDatabase(const std::string &backup_path);

        // Session pool tuning
        void setSessionAcquireTimeoutMs(int ms);
        void setSessionAcquireBackoffMs(int ms);

    private:
        // Session management
        std::unique_ptr<SessionManager> session_manager_;
        std::string db_path_;
        bool connected_;
        Poco::Logger &logger_;
        std::shared_ptr<UnifiedObservableConfigManager> config_manager_;

        /**
         * @brief Create media files table
         * @return true if successful, false otherwise
         */
        // removed

        /**
         * @brief Create file hashes table
         * @return true if successful, false otherwise
         */
        // removed

        /**
         * @brief Create metadata table
         * @return true if successful, false otherwise
         */
        // removed

        /**
         * @brief Execute SQL statement
         * @param sql SQL statement to execute
         * @return true if successful, false otherwise
         */
        bool executeSQL(const std::string &sql);

        /**
         * @brief Execute a scalar query to check if a table exists
         */
        bool tableExists(const std::string &table_name);

        /**
         * @brief Read a text file into a string; return empty on failure
         */
        static std::string readTextFile(const std::string &path);

        // Ensure SessionManager exists and is initialized
        bool ensureSessionManagerReady();

        /**
         * @brief Log database error
         * @param operation Operation that failed
         * @param error Error message
         */
        void logDatabaseError(const std::string &operation, const std::string &error);
    };

} // namespace MediaDedup
