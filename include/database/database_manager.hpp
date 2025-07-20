#pragma once

#include "database/database_access_queue.hpp"
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <functional>
#include <sqlite3.h>
#include "media_processor.hpp"

// Forward declaration
class DatabaseAccessQueue;

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
    static DatabaseManager &getInstance(const std::string &db_path = "");
    static void resetForTesting(); // For test isolation
    static void shutdown();        // For proper cleanup
    DatabaseManager(const DatabaseManager &) = delete;
    DatabaseManager &operator=(const DatabaseManager &) = delete;
    DatabaseManager(DatabaseManager &&) = delete;
    DatabaseManager &operator=(DatabaseManager &&) = delete;

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
     * @brief Store a processing result in the database and return operation ID
     * @return Pair of DBOpResult and operation ID for tracking
     */
    std::pair<DBOpResult, size_t> storeProcessingResultWithId(const std::string &file_path, DedupMode mode, const ProcessingResult &result);

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
     * @brief Store a scanned file in the database and return operation ID
     * @param file_path Path to the scanned file
     * @param onFileNeedsProcessing Optional callback to trigger processing when file needs processing
     * @return Pair of DBOpResult and operation ID for tracking
     */
    std::pair<DBOpResult, size_t> storeScannedFileWithId(const std::string &file_path,
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
     * @brief Update the hash for a file after processing and return operation ID
     * @param file_path Path to the file
     * @param file_hash Hash of the file
     * @return Pair of DBOpResult and operation ID for tracking
     */
    std::pair<DBOpResult, size_t> updateFileHashWithId(const std::string &file_path, const std::string &file_hash);

    /**
     * @brief Check if a file exists in the scanned_files table
     * @param file_path Path to the file to check
     * @return true if file exists in database, false otherwise
     */
    bool fileExistsInDatabase(const std::string &file_path);

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
     * @brief Set links for a scanned file (for duplicate detection)
     * @param file_path Path to the file
     * @param linked_ids Vector of IDs that this file is linked to (duplicates)
     * @return DBOpResult indicating success or failure
     */
    DBOpResult setFileLinks(const std::string &file_path, const std::vector<int> &linked_ids);

    /**
     * @brief Get links for a scanned file
     * @param file_path Path to the file
     * @return Vector of linked file IDs
     */
    std::vector<int> getFileLinks(const std::string &file_path);

    /**
     * @brief Add a link to a scanned file
     * @param file_path Path to the file
     * @param linked_id ID of the file to link to
     * @return DBOpResult indicating success or failure
     */
    DBOpResult addFileLink(const std::string &file_path, int linked_id);

    /**
     * @brief Remove a link from a scanned file
     * @param file_path Path to the file
     * @param linked_id ID of the file to unlink from
     * @return DBOpResult indicating success or failure
     */
    DBOpResult removeFileLink(const std::string &file_path, int linked_id);

    /**
     * @brief Get all files that are linked to a specific file
     * @param file_path Path to the file
     * @return Vector of file paths that are linked to the specified file
     */
    std::vector<std::string> getLinkedFiles(const std::string &file_path);

    /**
     * @brief Check if the database connection is valid
     * @return true if database is initialized and connected, false otherwise
     */
    bool isValid();

    /**
     * @brief Store a user input in the database
     * @param input_type Type of input (e.g., "scan_path", "config_setting", etc.)
     * @param input_value The actual input value
     * @return DBOpResult indicating success or failure
     */
    DBOpResult storeUserInput(const std::string &input_type, const std::string &input_value);

    /**
     * @brief Get all user inputs of a specific type
     * @param input_type Type of input to retrieve
     * @return Vector of input values for the specified type
     */
    std::vector<std::string> getUserInputs(const std::string &input_type);

    /**
     * @brief Get all user inputs
     * @return Vector of pairs containing input type and value
     */
    std::vector<std::pair<std::string, std::string>> getAllUserInputs();

    /**
     * @brief Clear all user inputs
     * @return DBOpResult with success flag and error message
     */
    DBOpResult clearAllUserInputs();

    /**
     * @brief Wait for all pending database writes to complete
     */
    void waitForWrites();

    // Get access to the write queue for checking operation results
    const DatabaseAccessQueue &getAccessQueue() const { return *access_queue_; }

    // Check if the last database operation succeeded
    bool checkLastOperationSuccess();

private:
    /**
     * @brief Constructor - initializes database connection and tables
     * @param db_path Path to SQLite database file
     */
    explicit DatabaseManager(const std::string &db_path);
    sqlite3 *db_;
    std::string db_path_;
    std::unique_ptr<DatabaseAccessQueue> access_queue_;

    // Make db_ accessible to DatabaseAccessQueue
    friend class DatabaseAccessQueue;

    // Initialization
    void initialize();
    bool createMediaProcessingResultsTable();
    bool createScannedFilesTable();
    bool createUserInputsTable();

    // SQL helpers
    /**
     * @brief Execute a SQL statement
     * @return DBOpResult with success flag and error message
     */
    DBOpResult executeStatement(const std::string &sql);
    std::string resultToJson(const ProcessingResult &result);
    ProcessingResult jsonToResult(const std::string &json_str);

    static std::unique_ptr<DatabaseManager> instance_;
    static std::mutex instance_mutex_;

public:
    // Queue initialization utility (public for testing)
    bool waitForQueueInitialization(int max_retries = 5, int retry_delay_ms = 1000);

private:
};