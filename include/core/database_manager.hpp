#pragma once

#include <string>
#include <vector>
#include <memory>
#include <sqlite3.h>
#include "media_processor.hpp"

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
     * @brief Store media processing result
     * @param file_path Original file path
     * @param mode Processing mode used
     * @param result Processing result
     * @return true if successful, false otherwise
     */
    bool storeProcessingResult(const std::string &file_path,
                               DedupMode mode,
                               const ProcessingResult &result);

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
     * @brief Clear all processing results
     * @return true if successful, false otherwise
     */
    bool clearAllResults();

    /**
     * @brief Store a scanned file (just metadata - name and path)
     * @param file_path Full path to the file
     * @return true if successful, false otherwise
     */
    bool storeScannedFile(const std::string &file_path);

    /**
     * @brief Get all scanned files
     * @return Vector of pairs (file_path, file_name)
     */
    std::vector<std::pair<std::string, std::string>> getAllScannedFiles();

    /**
     * @brief Clear all scanned files
     * @return true if successful, false otherwise
     */
    bool clearAllScannedFiles();

    /**
     * @brief Check if the database connection is valid
     * @return true if database is initialized and connected, false otherwise
     */
    bool isValid() const;

    /**
     * @brief Mark a scanned file as processed
     * @param file_path Full path to the file
     * @return true if successful, false otherwise
     */
    bool markFileAsProcessed(const std::string &file_path);

    /**
     * @brief Get all unprocessed scanned files
     * @return Vector of pairs (file_path, file_name)
     */
    std::vector<std::pair<std::string, std::string>> getAllUnprocessedScannedFiles();

private:
    sqlite3 *db_;
    std::string db_path_;

    // Initialization
    void initialize();
    bool createMediaProcessingResultsTable();
    bool createScannedFilesTable();

    // SQL helpers
    bool executeStatement(const std::string &sql);
    std::string resultToJson(const ProcessingResult &result);
    ProcessingResult jsonToResult(const std::string &json_str);
};