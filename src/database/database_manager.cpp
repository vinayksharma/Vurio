#include "database/database_manager.hpp"
#include "database/database_access_queue.hpp"
#include "logging/logger.hpp"
#include "core/file_utils.hpp"
#include <nlohmann/json.hpp>
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <thread>
#include <mutex>

using json = nlohmann::json;

std::unique_ptr<DatabaseManager> DatabaseManager::instance_ = nullptr;
std::mutex DatabaseManager::instance_mutex_;

DatabaseManager &DatabaseManager::getInstance(const std::string &db_path)
{
    std::lock_guard<std::mutex> lock(instance_mutex_);
    if (!instance_)
    {
        if (db_path.empty())
        {
            throw std::runtime_error("DatabaseManager::getInstance called with empty db_path for first initialization");
        }
        instance_ = std::unique_ptr<DatabaseManager>(new DatabaseManager(db_path));
    }
    else if (instance_->db_path_ != db_path)
    {
        Logger::warn("DatabaseManager singleton already initialized with different path: " +
                     instance_->db_path_ + " vs " + db_path);
    }
    return *instance_;
}

void DatabaseManager::resetForTesting()
{
    std::lock_guard<std::mutex> lock(instance_mutex_);
    if (instance_)
    {
        instance_->waitForWrites(); // Ensure all writes complete
        instance_.reset();
    }
}

void DatabaseManager::shutdown()
{
    std::lock_guard<std::mutex> lock(instance_mutex_);
    if (instance_)
    {
        instance_->waitForWrites(); // Ensure all writes complete
        instance_.reset();
    }
}

DatabaseManager::DatabaseManager(const std::string &db_path)
    : db_(nullptr), db_path_(db_path)
{
    Logger::info("DatabaseManager constructor called for: " + db_path);
    // Initialize the access queue first
    access_queue_ = std::make_unique<DatabaseAccessQueue>(*this);

    // Enqueue the open operation and wait for it to complete
    auto open_future = access_queue_->enqueueRead([db_path](DatabaseManager &dbMan)
                                                  {
        int rc = sqlite3_open(db_path.c_str(), &dbMan.db_);
        if (rc != SQLITE_OK)
        {
            Logger::error("Failed to open database: " + std::string(sqlite3_errmsg(dbMan.db_)));
            sqlite3_close(dbMan.db_);
            dbMan.db_ = nullptr;
            return false;
        }
        Logger::info("Database opened successfully: " + db_path);
        // Enable WAL mode for better concurrency
        rc = sqlite3_exec(dbMan.db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
        if (rc != SQLITE_OK)
        {
            Logger::warn("Failed to enable WAL mode: " + std::string(sqlite3_errmsg(dbMan.db_)));
        }
        else
        {
            Logger::info("WAL mode enabled for database: " + db_path);
        }
        // Set additional PRAGMA settings for better concurrency
        sqlite3_exec(dbMan.db_, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
        sqlite3_exec(dbMan.db_, "PRAGMA cache_size=10000;", nullptr, nullptr, nullptr);
        sqlite3_exec(dbMan.db_, "PRAGMA temp_store=MEMORY;", nullptr, nullptr, nullptr);
        return true; });
    bool open_success = false;
    try
    {
        open_success = std::any_cast<bool>(open_future.get());
    }
    catch (...)
    {
        open_success = false;
    }
    if (!open_success)
    {
        Logger::error("Database open failed in access queue");
        return;
    }
    initialize();
    Logger::info("DatabaseManager initialization completed");
}

DatabaseManager::~DatabaseManager()
{
    Logger::info("DatabaseManager destructor called");
    if (access_queue_)
    {
        // Enqueue the close operation and wait for it to complete
        auto close_future = access_queue_->enqueueRead([](DatabaseManager &dbMan)
                                                       {
            if (dbMan.db_)
            {
                sqlite3_close(dbMan.db_);
                dbMan.db_ = nullptr;
                Logger::info("Database connection closed");
            }
            return true; });
        try
        {
            close_future.get();
        }
        catch (...)
        {
        }
        access_queue_->stop();
    }
}

void DatabaseManager::waitForWrites()
{
    if (access_queue_)
    {
        Logger::debug("Waiting for database writes to complete");
        access_queue_->wait_for_completion();
        Logger::debug("Database writes completed");
    }
}

bool DatabaseManager::checkLastOperationSuccess()
{
    // For now, we'll assume success if we can reach this point
    // In a more sophisticated implementation, we could track operation results
    return true;
}

void DatabaseManager::initialize()
{
    Logger::info("Initializing database tables");
    if (!createMediaProcessingResultsTable())
        Logger::error("Failed to create media_processing_results table");
    if (!createScannedFilesTable())
        Logger::error("Failed to create scanned_files table");
    Logger::info("Database tables initialization completed");
}

bool DatabaseManager::createMediaProcessingResultsTable()
{
    const std::string sql = R"(
        CREATE TABLE IF NOT EXISTS media_processing_results (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            file_path TEXT NOT NULL,
            processing_mode TEXT NOT NULL,
            success BOOLEAN NOT NULL,
            error_message TEXT,
            artifact_format TEXT,
            artifact_hash TEXT,
            artifact_confidence REAL,
            artifact_metadata TEXT,
            artifact_data BLOB,
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            UNIQUE(file_path, processing_mode)
        )
    )";
    return executeStatement(sql).success;
}

bool DatabaseManager::createScannedFilesTable()
{
    const std::string sql = R"(
        CREATE TABLE IF NOT EXISTS scanned_files (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            file_path TEXT NOT NULL UNIQUE,
            file_name TEXT NOT NULL,
            hash TEXT,
            links TEXT,
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        )
    )";
    return executeStatement(sql).success;
}

DBOpResult DatabaseManager::storeProcessingResult(const std::string &file_path,
                                                  DedupMode mode,
                                                  const ProcessingResult &result)
{
    Logger::debug("storeProcessingResult called for: " + file_path);

    if (!access_queue_)
    {
        std::string msg = "Access queue not initialized";
        Logger::error(msg);
        return DBOpResult(false, msg);
    }

    // Capture parameters for async execution
    std::string captured_file_path = file_path;
    DedupMode captured_mode = mode;
    ProcessingResult captured_result = result;
    std::string error_msg;
    bool success = true;

    // Enqueue the write operation
    access_queue_->enqueueWrite([captured_file_path, captured_mode, captured_result, &error_msg, &success](DatabaseManager &dbMan)
                                {
        Logger::debug("Executing storeProcessingResult in write queue for: " + captured_file_path);
        
        if (!dbMan.db_)
        {
            error_msg = "Database not initialized";
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }
        
        const std::string insert_sql = R"(
            INSERT OR REPLACE INTO media_processing_results 
            (file_path, processing_mode, success, error_message, 
             artifact_format, artifact_hash, artifact_confidence, 
             artifact_metadata, artifact_data)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
        )";

        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare_v2(dbMan.db_, insert_sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            error_msg = "Failed to prepare statement: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }

        // Bind parameters
        sqlite3_bind_text(stmt, 1, captured_file_path.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, DedupModes::getModeName(captured_mode).c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 3, captured_result.success ? 1 : 0);

        if (captured_result.error_message.empty())
        {
            sqlite3_bind_null(stmt, 4);
        }
        else
        {
            sqlite3_bind_text(stmt, 4, captured_result.error_message.c_str(), -1, SQLITE_STATIC);
        }

        if (captured_result.artifact.format.empty())
        {
            sqlite3_bind_null(stmt, 5);
        }
        else
        {
            sqlite3_bind_text(stmt, 5, captured_result.artifact.format.c_str(), -1, SQLITE_STATIC);
        }

        if (captured_result.artifact.hash.empty())
        {
            sqlite3_bind_null(stmt, 6);
        }
        else
        {
            sqlite3_bind_text(stmt, 6, captured_result.artifact.hash.c_str(), -1, SQLITE_STATIC);
        }

        sqlite3_bind_double(stmt, 7, captured_result.artifact.confidence);

        if (captured_result.artifact.metadata.empty())
        {
            sqlite3_bind_null(stmt, 8);
        }
        else
        {
            sqlite3_bind_text(stmt, 8, captured_result.artifact.metadata.c_str(), -1, SQLITE_STATIC);
        }

        if (captured_result.artifact.data.empty())
        {
            sqlite3_bind_blob(stmt, 9, nullptr, 0, SQLITE_STATIC);
        }
        else
        {
            sqlite3_bind_blob(stmt, 9, captured_result.artifact.data.data(),
                              static_cast<int>(captured_result.artifact.data.size()), SQLITE_STATIC);
        }

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE)
        {
            error_msg = "Failed to insert result: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }

        Logger::info("Stored processing result for: " + captured_file_path);
        return WriteOperationResult(); });

    waitForWrites();
    if (!success)
        return DBOpResult(false, error_msg);
    return DBOpResult(true, "");
}

std::pair<DBOpResult, size_t> DatabaseManager::storeProcessingResultWithId(const std::string &file_path,
                                                                           DedupMode mode,
                                                                           const ProcessingResult &result)
{
    Logger::debug("storeProcessingResultWithId called for: " + file_path);

    if (!access_queue_)
    {
        std::string msg = "Access queue not initialized";
        Logger::error(msg);
        return {DBOpResult(false, msg), 0};
    }

    // Capture parameters for async execution
    std::string captured_file_path = file_path;
    DedupMode captured_mode = mode;
    ProcessingResult captured_result = result;
    std::string error_msg;
    bool success = true;

    // Enqueue the write operation and get the operation ID
    size_t operation_id = access_queue_->enqueueWrite([captured_file_path, captured_mode, captured_result, &error_msg, &success](DatabaseManager &dbMan)
                                                      {
        Logger::debug("Executing storeProcessingResult in write queue for: " + captured_file_path);
        
        if (!dbMan.db_)
        {
            error_msg = "Database not initialized";
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }
        
        const std::string insert_sql = R"(
            INSERT OR REPLACE INTO media_processing_results 
            (file_path, processing_mode, success, error_message, 
             artifact_format, artifact_hash, artifact_confidence, 
             artifact_metadata, artifact_data)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
        )";

        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare_v2(dbMan.db_, insert_sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            error_msg = "Failed to prepare statement: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }

        // Bind parameters
        sqlite3_bind_text(stmt, 1, captured_file_path.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, DedupModes::getModeName(captured_mode).c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 3, captured_result.success ? 1 : 0);

        if (captured_result.error_message.empty())
        {
            sqlite3_bind_null(stmt, 4);
        }
        else
        {
            sqlite3_bind_text(stmt, 4, captured_result.error_message.c_str(), -1, SQLITE_STATIC);
        }

        if (captured_result.artifact.format.empty())
        {
            sqlite3_bind_null(stmt, 5);
        }
        else
        {
            sqlite3_bind_text(stmt, 5, captured_result.artifact.format.c_str(), -1, SQLITE_STATIC);
        }

        if (captured_result.artifact.hash.empty())
        {
            sqlite3_bind_null(stmt, 6);
        }
        else
        {
            sqlite3_bind_text(stmt, 6, captured_result.artifact.hash.c_str(), -1, SQLITE_STATIC);
        }

        sqlite3_bind_double(stmt, 7, captured_result.artifact.confidence);

        if (captured_result.artifact.metadata.empty())
        {
            sqlite3_bind_null(stmt, 8);
        }
        else
        {
            sqlite3_bind_text(stmt, 8, captured_result.artifact.metadata.c_str(), -1, SQLITE_STATIC);
        }

        if (captured_result.artifact.data.empty())
        {
            sqlite3_bind_blob(stmt, 9, nullptr, 0, SQLITE_STATIC);
        }
        else
        {
            sqlite3_bind_blob(stmt, 9, captured_result.artifact.data.data(),
                              static_cast<int>(captured_result.artifact.data.size()), SQLITE_STATIC);
        }

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE)
        {
            error_msg = "Failed to insert result: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }

        Logger::info("Stored processing result for: " + captured_file_path);
        return WriteOperationResult(); });

    waitForWrites();
    if (!success)
        return {DBOpResult(false, error_msg), 0};
    return {DBOpResult(true, ""), operation_id};
}

std::vector<ProcessingResult> DatabaseManager::getProcessingResults(const std::string &file_path)
{
    Logger::debug("getProcessingResults called for: " + file_path);
    std::vector<ProcessingResult> results;

    if (!access_queue_)
    {
        Logger::error("Access queue not initialized");
        return results;
    }

    // Capture parameters for async execution
    std::string captured_file_path = file_path;

    // Enqueue the read operation
    auto future = access_queue_->enqueueRead([captured_file_path](DatabaseManager &dbMan)
                                             {
        Logger::debug("Executing getProcessingResults in access queue for: " + captured_file_path);
        
        if (!dbMan.db_)
        {
            Logger::error("Database not initialized");
            return std::any(std::vector<ProcessingResult>());
        }
        
        std::vector<ProcessingResult> results;
        const std::string select_sql = R"(
            SELECT processing_mode, success, error_message, 
                   artifact_format, artifact_hash, artifact_confidence, 
                   artifact_metadata, artifact_data
            FROM media_processing_results 
            WHERE file_path = ?
            ORDER BY processing_mode
        )";

        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare_v2(dbMan.db_, select_sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            Logger::error("Failed to prepare select statement: " + std::string(sqlite3_errmsg(dbMan.db_)));
            return std::any(results);
        }

        sqlite3_bind_text(stmt, 1, captured_file_path.c_str(), -1, SQLITE_STATIC);

        while (sqlite3_step(stmt) == SQLITE_ROW)
        {
            ProcessingResult result;

            result.success = sqlite3_column_int(stmt, 1) != 0;

            if (sqlite3_column_type(stmt, 2) != SQLITE_NULL)
            {
                result.error_message = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
            }

            if (sqlite3_column_type(stmt, 3) != SQLITE_NULL)
            {
                result.artifact.format = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));
            }

            if (sqlite3_column_type(stmt, 4) != SQLITE_NULL)
            {
                result.artifact.hash = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 4));
            }

            result.artifact.confidence = sqlite3_column_double(stmt, 5);

            if (sqlite3_column_type(stmt, 6) != SQLITE_NULL)
            {
                result.artifact.metadata = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 6));
            }

            if (sqlite3_column_type(stmt, 7) != SQLITE_NULL)
            {
                const void *blob_data = sqlite3_column_blob(stmt, 7);
                int blob_size = sqlite3_column_bytes(stmt, 7);
                result.artifact.data.assign(
                    static_cast<const uint8_t *>(blob_data),
                    static_cast<const uint8_t *>(blob_data) + blob_size);
            }

            results.push_back(result);
        }

        sqlite3_finalize(stmt);
        return std::any(results); });

    // Wait for the result
    try
    {
        results = std::any_cast<std::vector<ProcessingResult>>(future.get());
    }
    catch (const std::exception &e)
    {
        Logger::error("Failed to get processing results: " + std::string(e.what()));
    }

    return results;
}

std::vector<std::pair<std::string, ProcessingResult>> DatabaseManager::getAllProcessingResults()
{
    Logger::debug("getAllProcessingResults called");
    std::vector<std::pair<std::string, ProcessingResult>> results;

    if (!access_queue_)
    {
        Logger::error("Access queue not initialized");
        return results;
    }

    // Enqueue the read operation
    auto future = access_queue_->enqueueRead([](DatabaseManager &dbMan)
                                             {
        Logger::debug("Executing getAllProcessingResults in access queue");
        
        if (!dbMan.db_)
        {
            Logger::error("Database not initialized");
            return std::any(std::vector<std::pair<std::string, ProcessingResult>>());
        }
        
        std::vector<std::pair<std::string, ProcessingResult>> results;
        const std::string select_sql = R"(
            SELECT file_path, processing_mode, success, error_message, 
                   artifact_format, artifact_hash, artifact_confidence, 
                   artifact_metadata, artifact_data
            FROM media_processing_results 
            ORDER BY file_path, processing_mode
        )";

        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare_v2(dbMan.db_, select_sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            Logger::error("Failed to prepare select statement: " + std::string(sqlite3_errmsg(dbMan.db_)));
            return std::any(results);
        }

        while (sqlite3_step(stmt) == SQLITE_ROW)
        {
            std::string file_path = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
            ProcessingResult result;

            result.success = sqlite3_column_int(stmt, 2) != 0;

            if (sqlite3_column_type(stmt, 3) != SQLITE_NULL)
            {
                result.error_message = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));
            }

            if (sqlite3_column_type(stmt, 4) != SQLITE_NULL)
            {
                result.artifact.format = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 4));
            }

            if (sqlite3_column_type(stmt, 5) != SQLITE_NULL)
            {
                result.artifact.hash = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 5));
            }

            result.artifact.confidence = sqlite3_column_double(stmt, 6);

            if (sqlite3_column_type(stmt, 7) != SQLITE_NULL)
            {
                result.artifact.metadata = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 7));
            }

            if (sqlite3_column_type(stmt, 8) != SQLITE_NULL)
            {
                const void *blob_data = sqlite3_column_blob(stmt, 8);
                int blob_size = sqlite3_column_bytes(stmt, 8);
                result.artifact.data.assign(
                    static_cast<const uint8_t *>(blob_data),
                    static_cast<const uint8_t *>(blob_data) + blob_size);
            }

            results.emplace_back(file_path, result);
        }

        sqlite3_finalize(stmt);
        return std::any(results); });

    // Wait for the result
    try
    {
        results = std::any_cast<std::vector<std::pair<std::string, ProcessingResult>>>(future.get());
    }
    catch (const std::exception &e)
    {
        Logger::error("Failed to get all processing results: " + std::string(e.what()));
    }

    return results;
}

DBOpResult DatabaseManager::clearAllResults()
{
    if (!access_queue_)
    {
        std::string msg = "Access queue not initialized";
        Logger::error(msg);
        return DBOpResult(false, msg);
    }

    std::string error_msg;
    bool success = true;

    // Enqueue the write operation
    access_queue_->enqueueWrite([&error_msg, &success](DatabaseManager &dbMan)
                                {
        if (!dbMan.db_)
        {
            error_msg = "Database not initialized";
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }
        
        const std::string delete_sql = "DELETE FROM media_processing_results";
        char *err_msg = nullptr;
        int rc = sqlite3_exec(dbMan.db_, delete_sql.c_str(), nullptr, nullptr, &err_msg);

        if (rc != SQLITE_OK)
        {
            error_msg = "SQL execution failed: " + std::string(err_msg);
            Logger::error(error_msg);
            sqlite3_free(err_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }

        return WriteOperationResult(true); });

    waitForWrites();
    if (!success)
        return DBOpResult(false, error_msg);
    return DBOpResult(true);
}

DBOpResult DatabaseManager::executeStatement(const std::string &sql)
{
    if (!access_queue_)
    {
        std::string msg = "Access queue not initialized";
        Logger::error(msg);
        return DBOpResult(false, msg);
    }
    std::string error_msg;
    bool success = true;
    access_queue_->enqueueWrite([&](DatabaseManager &dbMan)
                                {
        if (!dbMan.db_)
        {
            error_msg = "Database not initialized";
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }
        char *err_msg = nullptr;
        int rc = sqlite3_exec(dbMan.db_, sql.c_str(), nullptr, nullptr, &err_msg);
        if (rc != SQLITE_OK)
        {
            error_msg = "SQL execution failed: " + std::string(err_msg);
            Logger::error(error_msg);
            sqlite3_free(err_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }
        return WriteOperationResult(true); });
    waitForWrites();
    if (!success)
        return DBOpResult(false, error_msg);
    return DBOpResult(true);
}

std::string DatabaseManager::resultToJson(const ProcessingResult &result)
{
    json j;
    j["success"] = result.success;
    j["error_message"] = result.error_message;
    j["artifact"]["format"] = result.artifact.format;
    j["artifact"]["hash"] = result.artifact.hash;
    j["artifact"]["confidence"] = result.artifact.confidence;
    j["artifact"]["metadata"] = result.artifact.metadata;

    // Convert binary data to hex string
    std::stringstream ss;
    for (uint8_t byte : result.artifact.data)
    {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    }
    j["artifact"]["data"] = ss.str();

    return j.dump();
}

ProcessingResult DatabaseManager::jsonToResult(const std::string &json_str)
{
    ProcessingResult result;

    try
    {
        json j = json::parse(json_str);
        result.success = j["success"];
        result.error_message = j["error_message"];
        result.artifact.format = j["artifact"]["format"];
        result.artifact.hash = j["artifact"]["hash"];
        result.artifact.confidence = j["artifact"]["confidence"];
        result.artifact.metadata = j["artifact"]["metadata"];

        // Convert hex string back to binary data
        std::string hex_data = j["artifact"]["data"];
        for (size_t i = 0; i < hex_data.length(); i += 2)
        {
            std::string byte_str = hex_data.substr(i, 2);
            uint8_t byte = static_cast<uint8_t>(std::stoi(byte_str, nullptr, 16));
            result.artifact.data.push_back(byte);
        }
    }
    catch (const std::exception &e)
    {
        Logger::error("Failed to parse JSON result: " + std::string(e.what()));
    }

    return result;
}

DBOpResult DatabaseManager::storeScannedFile(const std::string &file_path,
                                             std::function<void(const std::string &)> onFileNeedsProcessing)
{
    if (!access_queue_)
    {
        std::string msg = "Access queue not initialized";
        Logger::error(msg);
        return DBOpResult(false, msg);
    }

    std::filesystem::path path(file_path);
    std::string file_name = path.filename().string();

    // Capture parameters for async execution
    std::string captured_file_path = file_path;
    std::string captured_file_name = file_name;
    auto captured_callback = onFileNeedsProcessing;
    std::string error_msg;
    bool success = true;

    // Compute file hash BEFORE enqueueing to avoid blocking the database queue
    std::string current_hash = FileUtils::computeFileHash(captured_file_path);

    // Enqueue the write operation
    access_queue_->enqueueWrite([captured_file_path, captured_file_name, captured_callback, current_hash, &error_msg, &success](DatabaseManager &dbMan)
                                {
        if (!dbMan.db_)
        {
            error_msg = "Database not initialized";
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }
        
        // Check if file already exists
        const std::string select_sql = "SELECT hash FROM scanned_files WHERE file_path = ?";
        sqlite3_stmt *select_stmt;
        int rc = sqlite3_prepare_v2(dbMan.db_, select_sql.c_str(), -1, &select_stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            error_msg = "Failed to prepare select statement: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }
        sqlite3_bind_text(select_stmt, 1, captured_file_path.c_str(), -1, SQLITE_STATIC);
        rc = sqlite3_step(select_stmt);
        if (rc == SQLITE_ROW)
        {
            // File exists, check if hash exists
            if (sqlite3_column_type(select_stmt, 0) != SQLITE_NULL)
            {
                // Hash exists, compare with current file hash (already computed)
                std::string existing_hash = reinterpret_cast<const char *>(sqlite3_column_text(select_stmt, 0));

                if (existing_hash == current_hash)
                {
                    // Hash matches, skip this file
                    sqlite3_finalize(select_stmt);
                    Logger::debug("File hash matches, skipping: " + captured_file_path);
                    return WriteOperationResult(true);
                }
                else
                {
                    // Hash differs, clear the hash and update timestamp
                    sqlite3_finalize(select_stmt);
                    const std::string update_sql = "UPDATE scanned_files SET hash = NULL, created_at = CURRENT_TIMESTAMP WHERE file_path = ?";
                    sqlite3_stmt *update_stmt;
                    rc = sqlite3_prepare_v2(dbMan.db_, update_sql.c_str(), -1, &update_stmt, nullptr);
                    if (rc != SQLITE_OK)
                    {
                        error_msg = "Failed to prepare update statement: " + std::string(sqlite3_errmsg(dbMan.db_));
                        Logger::error(error_msg);
                        success = false;
                        return WriteOperationResult::Failure(error_msg);
                    }
                    sqlite3_bind_text(update_stmt, 1, captured_file_path.c_str(), -1, SQLITE_STATIC);
                    rc = sqlite3_step(update_stmt);
                    sqlite3_finalize(update_stmt);
                    if (rc != SQLITE_DONE)
                    {
                        error_msg = "Failed to clear file hash: " + std::string(sqlite3_errmsg(dbMan.db_));
                        Logger::error(error_msg);
                        success = false;
                        return WriteOperationResult::Failure(error_msg);
                    }
                    Logger::info("File hash changed, cleared hash for: " + captured_file_path);
                    if (captured_callback)
                    {
                        captured_callback(captured_file_path);
                    }
                    return WriteOperationResult(true);
                }
            }
            else
            {
                // No hash, file needs processing
                sqlite3_finalize(select_stmt);
                Logger::info("File exists but has no hash, needs processing: " + captured_file_path);
                if (captured_callback)
                {
                    captured_callback(captured_file_path);
                }
                return WriteOperationResult(true);
            }
        }
        else
        {
            // File doesn't exist, insert it
            sqlite3_finalize(select_stmt);
            const std::string insert_sql = "INSERT INTO scanned_files (file_path, file_name) VALUES (?, ?)";
            sqlite3_stmt *insert_stmt;
            rc = sqlite3_prepare_v2(dbMan.db_, insert_sql.c_str(), -1, &insert_stmt, nullptr);
            if (rc != SQLITE_OK)
            {
                error_msg = "Failed to prepare insert statement: " + std::string(sqlite3_errmsg(dbMan.db_));
                Logger::error(error_msg);
                success = false;
                return WriteOperationResult::Failure(error_msg);
            }
            sqlite3_bind_text(insert_stmt, 1, captured_file_path.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(insert_stmt, 2, captured_file_name.c_str(), -1, SQLITE_STATIC);
            rc = sqlite3_step(insert_stmt);
            sqlite3_finalize(insert_stmt);
            if (rc != SQLITE_DONE)
            {
                error_msg = "Failed to insert scanned file: " + std::string(sqlite3_errmsg(dbMan.db_));
                Logger::error(error_msg);
                success = false;
                return WriteOperationResult::Failure(error_msg);
            }
            Logger::info("Stored new scanned file: " + captured_file_path);
            if (captured_callback)
            {
                captured_callback(captured_file_path);
            }
            return WriteOperationResult(true);
        } });

    waitForWrites();
    if (!success)
        return DBOpResult(false, error_msg);
    return DBOpResult(true);
}

// Get all scanned files
std::vector<std::pair<std::string, std::string>> DatabaseManager::getAllScannedFiles()
{
    Logger::debug("getAllScannedFiles called");
    std::vector<std::pair<std::string, std::string>> results;
    if (!access_queue_)
    {
        Logger::error("Access queue not initialized");
        return results;
    }

    // Enqueue the read operation
    auto future = access_queue_->enqueueRead([](DatabaseManager &dbMan)
                                             {
        Logger::debug("Executing getAllScannedFiles in access queue");
        
        if (!dbMan.db_)
        {
            Logger::error("Database not initialized");
            return std::any(std::vector<std::pair<std::string, std::string>>());
        }
        
        std::vector<std::pair<std::string, std::string>> results;
        const std::string select_sql = R"(
            SELECT file_path, file_name FROM scanned_files ORDER BY created_at DESC
        )";
        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare_v2(dbMan.db_, select_sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            Logger::error("Failed to prepare select statement: " + std::string(sqlite3_errmsg(dbMan.db_)));
            return std::any(results);
        }
        while (sqlite3_step(stmt) == SQLITE_ROW)
        {
            std::string file_path = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
            std::string file_name = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
            results.emplace_back(file_path, file_name);
        }
        sqlite3_finalize(stmt);
        return std::any(results); });

    // Wait for the result
    try
    {
        results = std::any_cast<std::vector<std::pair<std::string, std::string>>>(future.get());
    }
    catch (const std::exception &e)
    {
        Logger::error("Failed to get all scanned files: " + std::string(e.what()));
    }

    return results;
}

DBOpResult DatabaseManager::clearAllScannedFiles()
{
    if (!access_queue_)
    {
        std::string msg = "Access queue not initialized";
        Logger::error(msg);
        return DBOpResult(false, msg);
    }
    std::string error_msg;
    bool success = true;
    access_queue_->enqueueWrite([&](DatabaseManager &dbMan)
                                {
        if (!dbMan.db_)
        {
            error_msg = "Database not initialized";
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }
        const std::string delete_sql = "DELETE FROM scanned_files";
        char *err_msg = nullptr;
        int rc = sqlite3_exec(dbMan.db_, delete_sql.c_str(), nullptr, nullptr, &err_msg);
        if (rc != SQLITE_OK)
        {
            error_msg = "SQL execution failed: " + std::string(err_msg);
            Logger::error(error_msg);
            sqlite3_free(err_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }
        return WriteOperationResult(true); });
    waitForWrites();
    if (!success)
        return DBOpResult(false, error_msg);
    return DBOpResult(true);
}

// Get files that need processing (those without hash)
std::vector<std::pair<std::string, std::string>> DatabaseManager::getFilesNeedingProcessing()
{
    Logger::debug("getFilesNeedingProcessing called");
    std::vector<std::pair<std::string, std::string>> results;
    if (!access_queue_)
    {
        Logger::error("Access queue not initialized");
        return results;
    }

    // Enqueue the read operation
    auto future = access_queue_->enqueueRead([](DatabaseManager &dbMan)
                                             {
        Logger::debug("Executing getFilesNeedingProcessing in access queue");
        
        if (!dbMan.db_)
        {
            Logger::error("Database not initialized");
            return std::any(std::vector<std::pair<std::string, std::string>>());
        }
        
        std::vector<std::pair<std::string, std::string>> results;
        const std::string select_sql = R"(
            SELECT file_path, file_name FROM scanned_files WHERE hash IS NULL ORDER BY created_at DESC
        )";
        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare_v2(dbMan.db_, select_sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            Logger::error("Failed to prepare select statement: " + std::string(sqlite3_errmsg(dbMan.db_)));
            return std::any(results);
        }

        while (sqlite3_step(stmt) == SQLITE_ROW)
        {
            std::string file_path = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
            std::string file_name = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
            results.emplace_back(file_path, file_name);
        }

        sqlite3_finalize(stmt);
        return std::any(results); });

    // Wait for the result
    try
    {
        results = std::any_cast<std::vector<std::pair<std::string, std::string>>>(future.get());
    }
    catch (const std::exception &e)
    {
        Logger::error("Failed to get files needing processing: " + std::string(e.what()));
    }

    return results;
}

// Update the hash for a file after processing
DBOpResult DatabaseManager::updateFileHash(const std::string &file_path, const std::string &file_hash)
{
    if (!access_queue_)
    {
        std::string msg = "Access queue not initialized";
        Logger::error(msg);
        return DBOpResult(false, msg);
    }

    // Capture parameters for async execution
    std::string captured_file_path = file_path;
    std::string captured_file_hash = file_hash;
    std::string error_msg;
    bool success = true;

    // Enqueue the write operation
    access_queue_->enqueueWrite([captured_file_path, captured_file_hash, &error_msg, &success](DatabaseManager &dbMan)
                                {
        Logger::debug("Executing updateFileHash in write queue for: " + captured_file_path);
        
        const std::string update_sql = "UPDATE scanned_files SET hash = ? WHERE file_path = ?";
        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare_v2(dbMan.db_, update_sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            error_msg = "Failed to prepare update statement: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }

        sqlite3_bind_text(stmt, 1, captured_file_hash.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, captured_file_path.c_str(), -1, SQLITE_STATIC);

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE)
        {
            error_msg = "Failed to update file hash: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }

        Logger::debug("Updated file hash for: " + captured_file_path);
        return WriteOperationResult(); });

    waitForWrites();
    if (!success)
        return DBOpResult(false, error_msg);
    return DBOpResult(true, "");
}

std::pair<DBOpResult, size_t> DatabaseManager::updateFileHashWithId(const std::string &file_path, const std::string &file_hash)
{
    if (!access_queue_)
    {
        std::string msg = "Access queue not initialized";
        Logger::error(msg);
        return {DBOpResult(false, msg), 0};
    }

    // Capture parameters for async execution
    std::string captured_file_path = file_path;
    std::string captured_file_hash = file_hash;
    std::string error_msg;
    bool success = true;

    // Enqueue the write operation and get the operation ID
    size_t operation_id = access_queue_->enqueueWrite([captured_file_path, captured_file_hash, &error_msg, &success](DatabaseManager &dbMan)
                                                      {
        Logger::debug("Executing updateFileHash in write queue for: " + captured_file_path);
        
        const std::string update_sql = "UPDATE scanned_files SET hash = ? WHERE file_path = ?";
        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare_v2(dbMan.db_, update_sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            error_msg = "Failed to prepare update statement: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }

        sqlite3_bind_text(stmt, 1, captured_file_hash.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, captured_file_path.c_str(), -1, SQLITE_STATIC);

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE)
        {
            error_msg = "Failed to update file hash: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }

        Logger::debug("Updated file hash for: " + captured_file_path);
        return WriteOperationResult(); });

    waitForWrites();
    if (!success)
        return {DBOpResult(false, error_msg), 0};
    return {DBOpResult(true, ""), operation_id};
}

bool DatabaseManager::isValid()
{
    if (!access_queue_)
    {
        return false;
    }
    auto future = access_queue_->enqueueRead([](DatabaseManager &dbMan)
                                             { return dbMan.db_ != nullptr; });
    try
    {
        return std::any_cast<bool>(future.get());
    }
    catch (...)
    {
        return false;
    }
}

// Links management methods for duplicate detection

DBOpResult DatabaseManager::setFileLinks(const std::string &file_path, const std::vector<int> &linked_ids)
{
    if (!access_queue_)
    {
        std::string msg = "Access queue not initialized";
        Logger::error(msg);
        return DBOpResult(false, msg);
    }

    // Convert vector to JSON array
    json links_array = json::array();
    for (int id : linked_ids)
    {
        links_array.push_back(id);
    }
    std::string links_json = links_array.dump();

    // Capture parameters for async execution
    std::string captured_file_path = file_path;
    std::string captured_links_json = links_json;
    std::string error_msg;
    bool success = true;

    // Enqueue the write operation
    access_queue_->enqueueWrite([captured_file_path, captured_links_json, &error_msg, &success](DatabaseManager &dbMan)
                                {
        Logger::debug("Executing setFileLinks in write queue for: " + captured_file_path);
        
        const std::string update_sql = "UPDATE scanned_files SET links = ? WHERE file_path = ?";
        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare_v2(dbMan.db_, update_sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            error_msg = "Failed to prepare update statement: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }

        sqlite3_bind_text(stmt, 1, captured_links_json.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, captured_file_path.c_str(), -1, SQLITE_STATIC);

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE)
        {
            error_msg = "Failed to update file links: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }

        Logger::debug("Updated file links for: " + captured_file_path);
        return WriteOperationResult(); });

    waitForWrites();
    if (!success)
        return DBOpResult(false, error_msg);
    return DBOpResult(true, "");
}

std::vector<int> DatabaseManager::getFileLinks(const std::string &file_path)
{
    std::vector<int> results;
    if (!access_queue_)
    {
        Logger::error("Access queue not initialized");
        return results;
    }

    // Capture parameters for async execution
    std::string captured_file_path = file_path;

    // Enqueue the read operation
    auto future = access_queue_->enqueueRead([captured_file_path](DatabaseManager &dbMan)
                                             {
        Logger::debug("Executing getFileLinks in access queue for: " + captured_file_path);
        
        if (!dbMan.db_)
        {
            Logger::error("Database not initialized");
            return std::any(std::vector<int>());
        }
        
        std::vector<int> results;
        const std::string select_sql = "SELECT links FROM scanned_files WHERE file_path = ?";
        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare_v2(dbMan.db_, select_sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            Logger::error("Failed to prepare select statement: " + std::string(sqlite3_errmsg(dbMan.db_)));
            return std::any(results);
        }

        sqlite3_bind_text(stmt, 1, captured_file_path.c_str(), -1, SQLITE_STATIC);

        if (sqlite3_step(stmt) == SQLITE_ROW)
        {
            if (sqlite3_column_type(stmt, 0) != SQLITE_NULL)
            {
                std::string links_json = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
                try
                {
                    json links_array = json::parse(links_json);
                    for (const auto &id : links_array)
                    {
                        results.push_back(id.get<int>());
                    }
                }
                catch (const json::exception &e)
                {
                    Logger::error("Failed to parse links JSON: " + std::string(e.what()));
                }
            }
        }

        sqlite3_finalize(stmt);
        return std::any(results); });

    // Wait for the result
    try
    {
        results = std::any_cast<std::vector<int>>(future.get());
    }
    catch (const std::exception &e)
    {
        Logger::error("Failed to get file links: " + std::string(e.what()));
    }

    return results;
}

DBOpResult DatabaseManager::addFileLink(const std::string &file_path, int linked_id)
{
    // Get current links
    std::vector<int> current_links = getFileLinks(file_path);

    // Check if link already exists
    if (std::find(current_links.begin(), current_links.end(), linked_id) != current_links.end())
    {
        Logger::debug("Link already exists for file: " + file_path + " to ID: " + std::to_string(linked_id));
        return DBOpResult(true, ""); // Already linked
    }

    // Add new link
    current_links.push_back(linked_id);
    return setFileLinks(file_path, current_links);
}

DBOpResult DatabaseManager::removeFileLink(const std::string &file_path, int linked_id)
{
    // Get current links
    std::vector<int> current_links = getFileLinks(file_path);

    // Remove the link
    auto it = std::find(current_links.begin(), current_links.end(), linked_id);
    if (it == current_links.end())
    {
        Logger::debug("Link not found for file: " + file_path + " to ID: " + std::to_string(linked_id));
        return DBOpResult(true, ""); // Link doesn't exist
    }

    current_links.erase(it);
    return setFileLinks(file_path, current_links);
}

std::vector<std::string> DatabaseManager::getLinkedFiles(const std::string &file_path)
{
    std::vector<std::string> results;
    if (!access_queue_)
    {
        Logger::error("Access queue not initialized");
        return results;
    }

    // Get the ID of the current file first
    int current_id = -1;
    {
        auto future = access_queue_->enqueueRead([file_path](DatabaseManager &dbMan)
                                                 {
            Logger::debug("Getting file ID for: " + file_path);
            
            if (!dbMan.db_)
            {
                Logger::error("Database not initialized");
                return std::any(-1);
            }
            
            const std::string select_sql = "SELECT id FROM scanned_files WHERE file_path = ?";
            sqlite3_stmt *stmt;
            int rc = sqlite3_prepare_v2(dbMan.db_, select_sql.c_str(), -1, &stmt, nullptr);
            if (rc != SQLITE_OK)
            {
                Logger::error("Failed to prepare select statement: " + std::string(sqlite3_errmsg(dbMan.db_)));
                return std::any(-1);
            }

            sqlite3_bind_text(stmt, 1, file_path.c_str(), -1, SQLITE_STATIC);

            int id = -1;
            if (sqlite3_step(stmt) == SQLITE_ROW)
            {
                id = sqlite3_column_int(stmt, 0);
            }

            sqlite3_finalize(stmt);
            return std::any(id); });

        try
        {
            current_id = std::any_cast<int>(future.get());
        }
        catch (const std::exception &e)
        {
            Logger::error("Failed to get file ID: " + std::string(e.what()));
            return results;
        }
    }

    if (current_id == -1)
    {
        Logger::error("File not found: " + file_path);
        return results;
    }

    // Find all files that link to this file
    auto future = access_queue_->enqueueRead([current_id](DatabaseManager &dbMan)
                                             {
        Logger::debug("Finding files linked to ID: " + std::to_string(current_id));
        
        if (!dbMan.db_)
        {
            Logger::error("Database not initialized");
            return std::any(std::vector<std::string>());
        }
        
        std::vector<std::string> results;
        const std::string select_sql = "SELECT file_path FROM scanned_files WHERE links LIKE ?";
        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare_v2(dbMan.db_, select_sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            Logger::error("Failed to prepare select statement: " + std::string(sqlite3_errmsg(dbMan.db_)));
            return std::any(results);
        }

        // Search for files that contain the current ID in their links JSON
        std::string search_pattern = "%" + std::to_string(current_id) + "%";
        sqlite3_bind_text(stmt, 1, search_pattern.c_str(), -1, SQLITE_STATIC);

        while (sqlite3_step(stmt) == SQLITE_ROW)
        {
            std::string linked_file_path = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
            results.push_back(linked_file_path);
        }

        sqlite3_finalize(stmt);
        return std::any(results); });

    // Wait for the result
    try
    {
        results = std::any_cast<std::vector<std::string>>(future.get());
    }
    catch (const std::exception &e)
    {
        Logger::error("Failed to get linked files: " + std::string(e.what()));
    }

    return results;
}