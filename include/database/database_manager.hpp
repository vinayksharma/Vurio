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
#include <memory>
#include <vector>
#include <unordered_map>

namespace MediaDedup
{

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
         * @brief Constructor
         * @param db_path Path to SQLite database file
         */
        explicit DatabaseManager(const std::string &db_path = "media_dedup.db");

        /**
         * @brief Destructor
         */
        ~DatabaseManager();

        /**
         * @brief Initialize database connection
         * @return true if successful, false otherwise
         */
        bool initialize();

        /**
         * @brief Close database connection
         */
        void close();

        /**
         * @brief Create database tables
         * @return true if successful, false otherwise
         */
        bool createTables();

        /**
         * @brief Check if database is connected
         * @return true if connected, false otherwise
         */
        bool isConnected() const;

        /**
         * @brief Get database session
         * @return Reference to database session
         */
        Poco::Data::Session &getSession();

        // Media file operations
        /**
         * @brief Store media file information
         * @param file_path File path
         * @param file_hash File hash
         * @param file_size File size in bytes
         * @param file_type File type (image, video, audio)
         * @param metadata Additional metadata
         * @return true if successful, false otherwise
         */
        bool storeMediaFile(const std::string &file_path,
                            const std::string &file_hash,
                            uint64_t file_size,
                            const std::string &file_type,
                            const std::string &metadata = "");

        /**
         * @brief Get media file by hash
         * @param file_hash File hash to search for
         * @return Vector of file paths with matching hash
         */
        std::vector<std::string> getMediaFilesByHash(const std::string &file_hash);

        /**
         * @brief Get media file by path
         * @param file_path File path to search for
         * @return File hash if found, empty string otherwise
         */
        std::string getMediaFileHash(const std::string &file_path);

        /**
         * @brief Update media file metadata
         * @param file_path File path
         * @param metadata New metadata
         * @return true if successful, false otherwise
         */
        bool updateMediaFileMetadata(const std::string &file_path, const std::string &metadata);

        /**
         * @brief Delete media file record
         * @param file_path File path to delete
         * @return true if successful, false otherwise
         */
        bool deleteMediaFile(const std::string &file_path);

        // Duplicate detection operations
        /**
         * @brief Find duplicate files by hash
         * @param file_hash File hash to search for
         * @return Vector of duplicate file paths
         */
        std::vector<std::string> findDuplicatesByHash(const std::string &file_hash);

        /**
         * @brief Find all duplicate files
         * @return Map of hash to vector of file paths
         */
        std::unordered_map<std::string, std::vector<std::string>> findAllDuplicates();

        /**
         * @brief Get duplicate statistics
         * @return Map of hash to duplicate count
         */
        std::unordered_map<std::string, size_t> getDuplicateStatistics();

        // Database maintenance
        /**
         * @brief Vacuum database (reclaim space)
         * @return true if successful, false otherwise
         */
        bool vacuumDatabase();

        /**
         * @brief Get database statistics
         * @return Map of statistics
         */
        std::unordered_map<std::string, std::string> getDatabaseStats();

        /**
         * @brief Backup database
         * @param backup_path Path for backup file
         * @return true if successful, false otherwise
         */
        bool backupDatabase(const std::string &backup_path);

    private:
        std::string db_path_;
        bool connected_;
        Poco::Logger &logger_;
        std::unique_ptr<Poco::Data::Session> session_;

        /**
         * @brief Create media files table
         * @return true if successful, false otherwise
         */
        bool createMediaFilesTable();

        /**
         * @brief Create file hashes table
         * @return true if successful, false otherwise
         */
        bool createFileHashesTable();

        /**
         * @brief Create metadata table
         * @return true if successful, false otherwise
         */
        bool createMetadataTable();

        /**
         * @brief Execute SQL statement
         * @param sql SQL statement to execute
         * @return true if successful, false otherwise
         */
        bool executeSQL(const std::string &sql);

        /**
         * @brief Log database error
         * @param operation Operation that failed
         * @param error Error message
         */
        void logDatabaseError(const std::string &operation, const std::string &error);
    };

} // namespace MediaDedup
