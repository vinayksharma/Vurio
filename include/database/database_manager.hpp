#pragma once

#include "database/database_access_queue.hpp"
#include "core/processing_result.hpp"
#include "core/dedup_modes.hpp"
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <functional>
#include <sqlite3.h>

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
    static bool isTestMode();      // Check if running in test mode
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
     * @return DBOpResult with success flag and error message
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
     * @brief Get files that need processing for a specific mode
     * @param mode The processing mode
     * @return Vector of file paths and names that need processing
     */
    std::vector<std::pair<std::string, std::string>> getFilesNeedingProcessing(DedupMode mode);

    /**
     * @brief Atomically get and mark files as in progress (prevents duplicates)
     * @param mode The processing mode
     * @param batch_size Maximum number of files to get
     * @return Vector of file paths and names that are now marked as in progress
     */
    std::vector<std::pair<std::string, std::string>> getAndMarkFilesForProcessing(DedupMode mode, int batch_size = 10);
    std::vector<std::pair<std::string, std::string>> getFilesNeedingProcessingAnyMode(int batch_size = 10);
    std::vector<std::pair<std::string, std::string>> getAndMarkFilesForProcessingAnyMode(int batch_size = 10);

    /**
     * @brief Set processing flag for a specific mode after successful processing
     * @param file_path Path to the file
     * @param mode The deduplication mode that was processed
     * @return DBOpResult indicating success or failure
     */
    DBOpResult setProcessingFlag(const std::string &file_path, DedupMode mode);

    /**
     * @brief Reset processing flag for a specific mode after failed processing
     * @param file_path Path to the file
     * @param mode Processing mode
     * @return DBOpResult indicating success or failure
     */
    DBOpResult resetProcessingFlag(const std::string &file_path, DedupMode mode);

    /**
     * @brief Set processing flag to error state (2) for a specific mode
     * @param file_path Path to the file
     * @param mode Processing mode
     * @return DBOpResult indicating success or failure
     */
    DBOpResult setProcessingFlagError(const std::string &file_path, DedupMode mode);

    /**
     * @brief Set processing flag to transcoding error state (3) for a specific mode
     * @param file_path Path to the file
     * @param mode Processing mode
     * @return DBOpResult indicating success or failure
     */
    DBOpResult setProcessingFlagTranscodingError(const std::string &file_path, DedupMode mode);

    /**
     * @brief Set processing flag to final error state (4) for a specific mode
     * @param file_path Path to the file
     * @param mode Processing mode
     * @return DBOpResult indicating success or failure
     */
    DBOpResult setProcessingFlagFinalError(const std::string &file_path, DedupMode mode);

    /**
     * @brief Reset all processing flags from -1 (in progress) to 0 (not processed) on startup
     * This ensures a clean state when the server restarts
     * @return DBOpResult indicating success or failure
     */
    DBOpResult resetAllProcessingFlagsOnStartup();

    /**
     * @brief Get files with a specific processing flag value for a mode
     * @param flag_value The processing flag value to search for
     * @param mode The deduplication mode
     * @return Vector of file paths with the specified flag value
     */
    std::vector<std::string> getFilesWithProcessingFlag(int flag_value, DedupMode mode);

    /**
     * @brief Get the current processing flag value for a file and mode
     * @param file_path Path to the file
     * @param mode The deduplication mode
     * @return The processing flag value, or -1 if not found
     */
    int getProcessingFlag(const std::string &file_path, DedupMode mode);

    /**
     * @brief Get files that need processing for any mode
     * @return Vector of file paths and names that need processing for any mode
     */
    std::vector<std::pair<std::string, std::string>> getFilesNeedingProcessingAnyMode();

    /**
     * @brief Check if a file needs processing for a specific mode
     * @param file_path The file path to check
     * @param mode The deduplication mode to check
     * @return True if the file needs processing for this mode
     */
    bool fileNeedsProcessingForMode(const std::string &file_path, DedupMode mode);

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
     * @brief Update the metadata for a file after processing
     * @param file_path Path to the file
     * @param metadata_str Metadata string to store
     * @return DBOpResult indicating success or failure
     */
    DBOpResult updateFileMetadata(const std::string &file_path, const std::string &metadata_str);

    /**
     * @brief Update the metadata for a file after processing and return operation ID
     * @param file_path Path to the file
     * @param metadata_str Metadata string to store
     * @return Pair of DBOpResult and operation ID for tracking
     */
    std::pair<DBOpResult, size_t> updateFileMetadataWithId(const std::string &file_path, const std::string &metadata_str);

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
     * @brief Set links for a scanned file in a specific deduplication mode
     * @param file_path Path to the file
     * @param linked_ids Vector of IDs that this file is linked to (duplicates)
     * @param mode The deduplication mode (FAST, BALANCED, or QUALITY)
     * @return DBOpResult indicating success or failure
     */
    DBOpResult setFileLinksForMode(const std::string &file_path, const std::vector<int> &linked_ids, DedupMode mode);

    /**
     * @brief Get links for a scanned file
     * @param file_path Path to the file
     * @return Vector of linked file IDs
     */
    std::vector<int> getFileLinks(const std::string &file_path);

    /**
     * @brief Get links for a scanned file in a specific deduplication mode
     * @param file_path Path to the file
     * @param mode The deduplication mode (FAST, BALANCED, or QUALITY)
     * @return Vector of linked file IDs
     */
    std::vector<int> getFileLinksForMode(const std::string &file_path, DedupMode mode);

    /**
     * @brief Get links for a scanned file in the current server mode
     * @param file_path Path to the file
     * @return Vector of linked file IDs for the current deduplication mode
     */
    std::vector<int> getFileLinksForCurrentMode(const std::string &file_path);

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

    // Dedupe support helpers
    int getFileId(const std::string &file_path);
    long getMaxProcessingResultId();
    std::vector<std::tuple<long, std::string, std::string>>
    getNewSuccessfulResults(DedupMode mode, long last_seen_id);
    std::vector<std::pair<std::string, std::string>>
    getSuccessfulFileHashesForMode(DedupMode mode);
    std::vector<std::string>
    getAllFilePathsForHashAndMode(const std::string &artifact_hash, DedupMode mode);

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
     * @brief Get a hash of the contents of a database table
     * @param table_name Name of the table to hash
     * @return Pair of success flag and hash string, or error message
     */
    std::pair<bool, std::string> getTableHash(const std::string &table_name);

    /**
     * @brief Get a hash of all database tables combined
     * @return Pair of success flag and hash string, or error message
     */
    std::pair<bool, std::string> getDatabaseHash();

    /**
     * @brief Get a combined hash of tables relevant for duplicate detection
     * @return Pair of success flag and hash string, or error message
     */
    std::pair<bool, std::string> getDuplicateDetectionHash();

    /**
     * @brief Insert a file path that needs transcoding into cache_map
     * @param source_file_path Path to the source file that needs transcoding
     * @return DBOpResult with success flag and error message
     */
    DBOpResult insertTranscodingFile(const std::string &source_file_path);

    /**
     * @brief Update a cache_map record with the transcoded file path
     * @param source_file_path Path to the source file
     * @param transcoded_file_path Path to the transcoded file
     * @return DBOpResult with success flag and error message
     */
    DBOpResult updateTranscodedFilePath(const std::string &source_file_path, const std::string &transcoded_file_path);

    /**
     * @brief Get the transcoded file path for a source file
     * @param source_file_path Path to the source file
     * @return The transcoded file path, or empty string if not found
     */
    std::string getTranscodedFilePath(const std::string &source_file_path);

    /**
     * @brief Get all files that need transcoding from the cache_map table
     * @return Vector of file paths that need transcoding
     * @note This method dynamically builds the SQL query based on enabled RAW formats from ServerConfigManager
     * @note Only file extensions marked as enabled=true in the extended_support configuration are included
     * @note This makes the system truly configuration-driven - no hardcoded file extensions
     * @note JPG, PNG, and other non-RAW files are automatically filtered out
     */
    std::vector<std::string> getFilesNeedingTranscoding();

    /**
     * @brief Check if a file needs transcoding
     * @param source_file_path Path to the source file
     * @return True if the file needs transcoding
     */
    bool fileNeedsTranscoding(const std::string &source_file_path);

    /**
     * @brief Remove a transcoding record
     * @param source_file_path Path to the source file
     * @return DBOpResult with success flag and error message
     */
    DBOpResult removeTranscodingRecord(const std::string &source_file_path);

    /**
     * @brief Clear all transcoding records
     * @return DBOpResult with success flag and error message
     */
    DBOpResult clearAllTranscodingRecords();

    // Transcoding job management helpers (serialized via DatabaseAccessQueue)
    std::string claimNextTranscodingJob();
    bool markTranscodingJobInProgress(const std::string &source_file_path);
    bool markTranscodingJobCompleted(const std::string &source_file_path, const std::string &transcoded_file_path);
    bool markTranscodingJobFailed(const std::string &source_file_path);

    /**
     * @brief Wait for all pending write operations to complete
     */
    void waitForWrites();

    /**
     * @brief Check if the last operation was successful
     * @return true if last operation succeeded, false otherwise
     */
    bool checkLastOperationSuccess();

    /**
     * @brief Atomically check if file needs processing and set processing flag in a single operation
     * This prevents race conditions by using a single SQL UPDATE with WHERE clause
     * @param file_path Path to the file to check and lock
     * @param mode The deduplication mode to check
     * @return true if file needs processing and lock was acquired, false otherwise
     */
    bool tryAcquireProcessingLock(const std::string &file_path, DedupMode mode);

    /**
     * @brief Get the boolean value of a named flag from the flags table
     */
    bool getFlag(const std::string &flag_name);

    /**
     * @brief Set a flag in the database
     * @param flag_name Name of the flag
     * @param value Boolean value to set
     * @return DBOpResult indicating success or failure
     */
    DBOpResult setFlag(const std::string &flag_name, bool value);

    /**
     * @brief Get a TEXT flag value from the database
     * @param flag_name Name of the flag
     * @return String value of the flag, or empty string if not found
     */
    std::string getTextFlag(const std::string &flag_name);

    /**
     * @brief Set a TEXT flag value in the database
     * @param flag_name Name of the flag
     * @param value String value to set
     * @return DBOpResult indicating success or failure
     */
    DBOpResult setTextFlag(const std::string &flag_name, const std::string &value);

    /**
     * @brief Get server status metrics from database
     * @return Struct containing counts of scanned files, queued files, processed files, and duplicates
     */
    struct ServerStatus
    {
        size_t files_scanned;
        size_t files_queued;
        size_t files_processed;
        size_t duplicates_found;
        size_t files_in_error;
    };
    ServerStatus getServerStatus();

private:
    /**
     * @brief Constructor - initializes database connection and tables
     * @param db_path Path to SQLite database file
     */
    explicit DatabaseManager(const std::string &db_path);
    sqlite3 *db_;
    std::string db_path_;
    std::unique_ptr<DatabaseAccessQueue> access_queue_;
    std::mutex queue_check_mutex;
    std::mutex file_processing_mutex; // Mutex for file processing operations to prevent race conditions

    // Make db_ accessible to DatabaseAccessQueue
    friend class DatabaseAccessQueue;

    // Make db_ accessible to TranscodingManager for database-only transcoding
    friend class TranscodingManager;

    // Initialization
    void initialize();
    bool createMediaProcessingResultsTable();
    bool createScannedFilesTable();
    bool createUserInputsTable();
    bool createCacheMapTable();
    bool createTranscodingTable();
    bool createFlagsTable();
    bool createScannedFilesChangeTriggers();

    // SQL helpers
    /**
     * @brief Execute a SQL statement
     * @return DBOpResult with success flag and error message
     */
    DBOpResult executeStatement(const std::string &sql);
    std::string resultToJson(const ProcessingResult &result);
    ProcessingResult jsonToResult(const std::string &json_str);

    // Helper function to generate SQL LIKE clauses for enabled file types
    std::string generateFileTypeLikeClauses();

    static std::unique_ptr<DatabaseManager> instance_;
    static std::mutex instance_mutex_;

public:
    // Queue initialization utility (public for testing)
    bool waitForQueueInitialization(int max_retries = 5, int retry_delay_ms = 1000);

private:
};