#pragma once

#include <string>
#include <vector>
#include <memory>
#include <sqlite3.h>
#include "media_processor.hpp"

/**
 * @brief Result of a database operation
 */
struct DBOpResult
{
    bool success;
    std::string error_message;
    DBOpResult(bool s = true, const std::string &msg = "") : success(s), error_message(msg) {}
};

/**
 * @brief SQLite database manager for storing media processing results
 */
class DatabaseManager
{
public:
    /**
     * @brief Constructor - initializes database connection and tables
     * @param db_path Path to SQLite database file
     */
    explicit DatabaseManager(const std::string &db_path);

    /**
     * @brief Destructor - closes database connection
     */
    ~DatabaseManager();

    /**
     * @brief Store a processing result in the database
     * @return DBOpResult with success flag and error message
     */
    DBOpResult storeProcessingResult(const std::string &file_path, DedupMode mode, const ProcessingResult &result);

    /**
     * @brief Get processing results for a file
     * @param file_path File path to query
     * @return Vector of processing results
     */
    std::vector<ProcessingResult> getProcessingResults(const std::string &file_path);

    /**
     * @brief Get all processing results
     * @return Vector of all processing results
     */
    std::vector<std::pair<std::string, ProcessingResult>> getAllProcessingResults();

    /**
     * @brief Clear all results
     * @return DBOpResult with success flag and error message
     */
    DBOpResult clearAllResults();

    /**
     * @brief Store a scanned file in the database
     * @param file_path Path to the scanned file
     * @param onFileNeedsProcessing Optional callback to trigger processing when file needs processing
     * @return DBOpResult indicating success or failure
     */
    DBOpResult storeScannedFile(const std::string &file_path,
                                std::function<void(const std::string &)> onFileNeedsProcessing = nullptr);

    /**
     * @brief Get files that need processing (those without hash)
     * @return Vector of file path and name pairs that need processing
     */
    std::vector<std::pair<std::string, std::string>> getFilesNeedingProcessing();

    /**
     * @brief Update the hash for a file after processing
     * @param file_path Path to the file
     * @param file_hash Hash of the file
     * @return DBOpResult indicating success or failure
     */
    DBOpResult updateFileHash(const std::string &file_path, const std::string &file_hash);

    /**
     * @brief Get all scanned files
     * @return Vector of file path and name pairs
     */
    std::vector<std::pair<std::string, std::string>> getAllScannedFiles();

    /**
     * @brief Clear all scanned files
     * @return DBOpResult with success flag and error message
     */
    DBOpResult clearAllScannedFiles();

    /**
     * @brief Check if the database connection is valid
     * @return true if database is initialized and connected, false otherwise
     */
    bool isValid() const;

private:
    sqlite3 *db_;
    std::string db_path_;

    // Initialization
    void initialize();
    bool createMediaProcessingResultsTable();
    bool createScannedFilesTable();

    // SQL helpers
    /**
     * @brief Execute a SQL statement
     * @return DBOpResult with success flag and error message
     */
    DBOpResult executeStatement(const std::string &sql);
    std::string resultToJson(const ProcessingResult &result);
    ProcessingResult jsonToResult(const std::string &json_str);
};