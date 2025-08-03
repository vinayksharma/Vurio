#include "database/database_manager.hpp"
#include "database/database_access_queue.hpp"
#include "logging/logger.hpp"
#include "core/file_utils.hpp"
#include "core/mount_manager.hpp"
#include <nlohmann/json.hpp>
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <thread>
#include <mutex>
#include <chrono>
#include <atomic>

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
        
        // Enable foreign key support
        rc = sqlite3_exec(dbMan.db_, "PRAGMA foreign_keys=ON;", nullptr, nullptr, nullptr);
        if (rc != SQLITE_OK)
        {
            Logger::warn("Failed to enable foreign keys: " + std::string(sqlite3_errmsg(dbMan.db_)));
        }
        else
        {
            Logger::info("Foreign key support enabled");
        }
        
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
    return access_queue_->checkLastOperationSuccess();
}

void DatabaseManager::initialize()
{
    Logger::info("Initializing database tables");
    if (!createScannedFilesTable())
        Logger::error("Failed to create scanned_files table");
    if (!createMediaProcessingResultsTable())
        Logger::error("Failed to create media_processing_results table");
    if (!createUserInputsTable())
        Logger::error("Failed to create user_inputs table");
    if (!createCacheMapTable())
        Logger::error("Failed to create cache_map table");
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
            UNIQUE(file_path, processing_mode),
            FOREIGN KEY (file_path) REFERENCES scanned_files(file_path) ON DELETE CASCADE
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
            relative_path TEXT,           -- For network mounts: share:relative/path
            share_name TEXT,              -- The share name (B, G, etc.)
            file_name TEXT NOT NULL,
            file_metadata TEXT,           -- File metadata for change detection (creation date, modification date, size)
            processed_fast BOOLEAN DEFAULT 0,      -- Processing flag for FAST mode
            processed_balanced BOOLEAN DEFAULT 0,  -- Processing flag for BALANCED mode
            processed_quality BOOLEAN DEFAULT 0,   -- Processing flag for QUALITY mode
            links TEXT,
            is_network_file BOOLEAN DEFAULT 0,  -- True if on network mount
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        )
    )";
    return executeStatement(sql).success;
}

bool DatabaseManager::createUserInputsTable()
{
    const std::string sql = R"(
        CREATE TABLE IF NOT EXISTS user_inputs (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            input_type TEXT NOT NULL,
            input_value TEXT NOT NULL,
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            UNIQUE(input_type, input_value)
        )
    )";
    return executeStatement(sql).success;
}

bool DatabaseManager::createCacheMapTable()
{
    const std::string sql = R"(
        CREATE TABLE IF NOT EXISTS cache_map (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            source_file_path TEXT NOT NULL UNIQUE,
            transcoded_file_path TEXT,
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            FOREIGN KEY (source_file_path) REFERENCES scanned_files(file_path) ON DELETE CASCADE
        )
    )";
    return executeStatement(sql).success;
}

DBOpResult DatabaseManager::storeProcessingResult(const std::string &file_path,
                                                  DedupMode mode,
                                                  const ProcessingResult &result)
{
    Logger::debug("storeProcessingResult called for: " + file_path);

    if (!waitForQueueInitialization())
    {
        std::string msg = "Access queue not initialized after retries";
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

        Logger::info("Stored processing result (basic) for: " + captured_file_path);
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

    if (!waitForQueueInitialization())
    {
        std::string msg = "Access queue not initialized after retries";
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

        Logger::info("Stored processing result (with ID) for: " + captured_file_path);
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

    if (!waitForQueueInitialization())
    {
        Logger::error("Access queue not initialized after retries");
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

    if (!waitForQueueInitialization())
    {
        Logger::error("Access queue not initialized after retries");
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
    if (!waitForQueueInitialization())
    {
        std::string msg = "Access queue not initialized after retries";
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
    if (!waitForQueueInitialization())
    {
        std::string msg = "Access queue not initialized after retries";
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
    if (!waitForQueueInitialization())
    {
        std::string msg = "Access queue not initialized after retries";
        Logger::error(msg);
        return DBOpResult(false, msg);
    }

    std::filesystem::path path(file_path);
    std::string file_name = path.filename().string();

    // Check if this is a network path and convert to relative path
    auto &mount_manager = MountManager::getInstance();
    bool is_network_file = mount_manager.isNetworkPath(file_path);
    std::string relative_path;
    std::string share_name;

    if (is_network_file)
    {
        auto relative = mount_manager.toRelativePath(file_path);
        if (relative)
        {
            relative_path = relative->share_name + ":" + relative->relative_path;
            share_name = relative->share_name;
            Logger::debug("Storing network file with relative path: " + relative_path);
        }
        else
        {
            Logger::warn("Failed to convert network path to relative: " + file_path);
        }
    }

    // Always compute metadata during scanning
    Logger::debug("Getting metadata for file: " + file_path);
    auto metadata = FileUtils::getFileMetadata(file_path);
    std::string current_metadata_str;
    if (metadata)
    {
        current_metadata_str = FileUtils::metadataToString(*metadata);
    }
    else
    {
        Logger::warn("Could not get metadata for file: " + file_path);
    }

    // Capture parameters for async execution
    std::string captured_file_path = file_path;
    std::string captured_file_name = file_name;
    std::string captured_relative_path = relative_path;
    std::string captured_share_name = share_name;
    bool captured_is_network = is_network_file;
    std::string captured_metadata_str = current_metadata_str;
    auto captured_callback = onFileNeedsProcessing;
    std::string error_msg;
    bool success = true;

    // Enqueue the write operation
    access_queue_->enqueueWrite([captured_file_path, captured_file_name, captured_relative_path, captured_share_name, captured_is_network, captured_metadata_str, captured_callback, &error_msg, &success](DatabaseManager &dbMan)
                                {
        if (!dbMan.db_)
        {
            error_msg = "Database not initialized";
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }
        
        // Check if file already exists
        const std::string select_sql = "SELECT file_metadata, processed_fast, processed_balanced, processed_quality FROM scanned_files WHERE file_path = ?";
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
            // File exists, check if metadata exists
            if (sqlite3_column_type(select_stmt, 0) != SQLITE_NULL)
            {
                // Metadata exists, compare with current metadata
                std::string existing_metadata_str = reinterpret_cast<const char *>(sqlite3_column_text(select_stmt, 0));
                
                // Parse existing metadata
                auto existing_metadata = FileUtils::metadataFromString(existing_metadata_str);
                auto current_metadata = FileUtils::metadataFromString(captured_metadata_str);
                
                if (existing_metadata && current_metadata && *existing_metadata == *current_metadata)
                {
                    // Metadata matches, file hasn't changed
                    sqlite3_finalize(select_stmt);
                    Logger::debug("File metadata matches, file unchanged: " + captured_file_path);
                    return WriteOperationResult(true);
                }
                else
                {
                    // Metadata differs, file has changed - clear all processing flags
                    sqlite3_finalize(select_stmt);
                    const std::string update_sql = "UPDATE scanned_files SET file_metadata = ?, processed_fast = 0, processed_balanced = 0, processed_quality = 0, created_at = CURRENT_TIMESTAMP WHERE file_path = ?";
                    sqlite3_stmt *update_stmt;
                    rc = sqlite3_prepare_v2(dbMan.db_, update_sql.c_str(), -1, &update_stmt, nullptr);
                    if (rc != SQLITE_OK)
                    {
                        error_msg = "Failed to prepare update statement: " + std::string(sqlite3_errmsg(dbMan.db_));
                        Logger::error(error_msg);
                        success = false;
                        return WriteOperationResult::Failure(error_msg);
                    }
                    sqlite3_bind_text(update_stmt, 1, captured_metadata_str.c_str(), -1, SQLITE_STATIC);
                    sqlite3_bind_text(update_stmt, 2, captured_file_path.c_str(), -1, SQLITE_STATIC);
                    rc = sqlite3_step(update_stmt);
                    sqlite3_finalize(update_stmt);
                    if (rc != SQLITE_DONE)
                    {
                        error_msg = "Failed to update file metadata and clear flags: " + std::string(sqlite3_errmsg(dbMan.db_));
                        Logger::error(error_msg);
                        success = false;
                        return WriteOperationResult::Failure(error_msg);
                    }
                    Logger::info("File metadata changed, cleared processing flags for: " + captured_file_path);
                    if (captured_callback)
                    {
                        captured_callback(captured_file_path);
                    }
                    return WriteOperationResult(true);
                }
            }
            else
            {
                // No metadata, file needs processing
                sqlite3_finalize(select_stmt);
                Logger::info("File exists but has no metadata, needs processing: " + captured_file_path);
                if (captured_callback)
                {
                    captured_callback(captured_file_path);
                }
                return WriteOperationResult(true);
            }
        }
        else
        {
            // File doesn't exist, insert it with metadata
            sqlite3_finalize(select_stmt);
            const std::string insert_sql = "INSERT INTO scanned_files (file_path, file_name, relative_path, share_name, is_network_file, file_metadata) VALUES (?, ?, ?, ?, ?, ?)";
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
            sqlite3_bind_text(insert_stmt, 3, captured_relative_path.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(insert_stmt, 4, captured_share_name.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_int(insert_stmt, 5, captured_is_network ? 1 : 0);
            sqlite3_bind_text(insert_stmt, 6, captured_metadata_str.c_str(), -1, SQLITE_STATIC);
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
    if (!waitForQueueInitialization())
    {
        Logger::error("Access queue not initialized after retries");
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

bool DatabaseManager::fileExistsInDatabase(const std::string &file_path)
{
    Logger::debug("fileExistsInDatabase called for: " + file_path);

    if (!waitForQueueInitialization())
    {
        Logger::error("Access queue not initialized after retries");
        return false;
    }

    // Check if this is a network path and try to resolve it
    auto &mount_manager = MountManager::getInstance();
    bool is_network_file = mount_manager.isNetworkPath(file_path);

    // Capture parameters for async execution
    std::string captured_file_path = file_path;
    bool captured_is_network = is_network_file;

    // Enqueue the read operation
    auto future = access_queue_->enqueueRead([captured_file_path, captured_is_network](DatabaseManager &dbMan)
                                             {
        Logger::debug("Executing fileExistsInDatabase in access queue for: " + captured_file_path);
        
        if (!dbMan.db_)
        {
            Logger::error("Database not initialized");
            return std::any(false);
        }
        
        // For network files, check both absolute path and relative path
        std::string select_sql;
        if (captured_is_network) {
            // Try to find by relative path first
            auto& mount_manager = MountManager::getInstance();
            auto relative = mount_manager.toRelativePath(captured_file_path);
            if (relative) {
                std::string relative_path = relative->share_name + ":" + relative->relative_path;
                select_sql = "SELECT COUNT(*) FROM scanned_files WHERE relative_path = ? OR file_path = ?";
            } else {
                select_sql = "SELECT COUNT(*) FROM scanned_files WHERE file_path = ?";
            }
        } else {
            select_sql = "SELECT COUNT(*) FROM scanned_files WHERE file_path = ?";
        }
        
        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare_v2(dbMan.db_, select_sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            Logger::error("Failed to prepare select statement: " + std::string(sqlite3_errmsg(dbMan.db_)));
            return std::any(false);
        }

        if (captured_is_network) {
            auto& mount_manager = MountManager::getInstance();
            auto relative = mount_manager.toRelativePath(captured_file_path);
            if (relative) {
                std::string relative_path = relative->share_name + ":" + relative->relative_path;
                sqlite3_bind_text(stmt, 1, relative_path.c_str(), -1, SQLITE_STATIC);
                sqlite3_bind_text(stmt, 2, captured_file_path.c_str(), -1, SQLITE_STATIC);
            } else {
                sqlite3_bind_text(stmt, 1, captured_file_path.c_str(), -1, SQLITE_STATIC);
            }
        } else {
            sqlite3_bind_text(stmt, 1, captured_file_path.c_str(), -1, SQLITE_STATIC);
        }
        
        bool exists = false;
        if (sqlite3_step(stmt) == SQLITE_ROW)
        {
            exists = (sqlite3_column_int(stmt, 0) > 0);
        }

        sqlite3_finalize(stmt);
        return std::any(exists); });

    // Wait for the result
    try
    {
        return std::any_cast<bool>(future.get());
    }
    catch (const std::exception &e)
    {
        Logger::error("Failed to check if file exists in database: " + std::string(e.what()));
        return false;
    }
}

DBOpResult DatabaseManager::clearAllScannedFiles()
{
    if (!waitForQueueInitialization())
    {
        std::string msg = "Access queue not initialized after retries";
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

// Get files that need processing for a specific mode
std::vector<std::pair<std::string, std::string>> DatabaseManager::getFilesNeedingProcessing(DedupMode current_mode)
{
    Logger::debug("getFilesNeedingProcessing called for mode: " + DedupModes::getModeName(current_mode));
    std::vector<std::pair<std::string, std::string>> results;
    if (!waitForQueueInitialization())
    {
        Logger::error("Access queue not initialized after retries");
        return results;
    }

    // Capture the current mode for async execution
    DedupMode captured_mode = current_mode;

    // Enqueue the read operation
    auto future = access_queue_->enqueueRead([captured_mode](DatabaseManager &dbMan)
                                             {
        Logger::debug("Executing getFilesNeedingProcessing in access queue for mode: " + DedupModes::getModeName(captured_mode));
        
        if (!dbMan.db_)
        {
            Logger::error("Database not initialized");
            return std::any(std::vector<std::pair<std::string, std::string>>());
        }
        
        std::vector<std::pair<std::string, std::string>> results;
        
        // Build the SQL query based on the mode
        std::string select_sql;
        switch (captured_mode)
        {
            case DedupMode::FAST:
                select_sql = R"(
                    SELECT file_path, file_name 
                    FROM scanned_files 
                    WHERE processed_fast = 0
                    ORDER BY created_at DESC
                )";
                break;
            case DedupMode::BALANCED:
                select_sql = R"(
                    SELECT file_path, file_name 
                    FROM scanned_files 
                    WHERE processed_balanced = 0
                    ORDER BY created_at DESC
                )";
                break;
            case DedupMode::QUALITY:
                select_sql = R"(
                    SELECT file_path, file_name 
                    FROM scanned_files 
                    WHERE processed_quality = 0
                    ORDER BY created_at DESC
                )";
                break;
            default:
                Logger::error("Unknown processing mode: " + DedupModes::getModeName(captured_mode));
                return std::any(results);
        }
        
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
    if (!waitForQueueInitialization())
    {
        std::string msg = "Access queue not initialized after retries";
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
    if (!waitForQueueInitialization())
    {
        std::string msg = "Access queue not initialized after retries";
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

DBOpResult DatabaseManager::storeUserInput(const std::string &input_type, const std::string &input_value)
{
    Logger::debug("storeUserInput called for type: " + input_type + ", value: " + input_value);

    if (!waitForQueueInitialization())
    {
        std::string msg = "Access queue not initialized after retries";
        Logger::error(msg);
        return DBOpResult(false, msg);
    }

    // Capture parameters for async execution
    std::string captured_input_type = input_type;
    std::string captured_input_value = input_value;
    std::string error_msg;
    bool success = true;

    // Enqueue the write operation
    access_queue_->enqueueWrite([captured_input_type, captured_input_value, &error_msg, &success](DatabaseManager &dbMan)
                                {
        Logger::debug("Executing storeUserInput in write queue for type: " + captured_input_type);
        
        if (!dbMan.db_)
        {
            error_msg = "Database not initialized";
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }
        
        const std::string insert_sql = R"(
            INSERT OR REPLACE INTO user_inputs (input_type, input_value)
            VALUES (?, ?)
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
        sqlite3_bind_text(stmt, 1, captured_input_type.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, captured_input_value.c_str(), -1, SQLITE_STATIC);

        // Execute the statement
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE)
        {
            error_msg = "Failed to insert user input: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            success = false;
            sqlite3_finalize(stmt);
            return WriteOperationResult::Failure(error_msg);
        }

        sqlite3_finalize(stmt);
        Logger::debug("User input stored successfully: " + captured_input_type + " = " + captured_input_value);
        return WriteOperationResult(); });

    waitForWrites();
    if (!success)
        return DBOpResult(false, error_msg);
    return DBOpResult(true, "");
}

std::vector<std::string> DatabaseManager::getUserInputs(const std::string &input_type)
{
    Logger::debug("getUserInputs called for type: " + input_type);
    std::vector<std::string> results;

    if (!waitForQueueInitialization())
    {
        Logger::error("Access queue not initialized after retries");
        return results;
    }

    // Enqueue the read operation
    auto future = access_queue_->enqueueRead([input_type](DatabaseManager &dbMan)
                                             {
        Logger::debug("Executing getUserInputs in access queue for type: " + input_type);
        
        if (!dbMan.db_)
        {
            Logger::error("Database not initialized");
            return std::any(std::vector<std::string>());
        }
        
        std::vector<std::string> results;
        const std::string select_sql = R"(
            SELECT input_value FROM user_inputs WHERE input_type = ? ORDER BY created_at DESC
        )";
        
        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare_v2(dbMan.db_, select_sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            Logger::error("Failed to prepare select statement: " + std::string(sqlite3_errmsg(dbMan.db_)));
            return std::any(results);
        }

        // Bind the input type parameter
        sqlite3_bind_text(stmt, 1, input_type.c_str(), -1, SQLITE_STATIC);

        while (sqlite3_step(stmt) == SQLITE_ROW)
        {
            std::string input_value = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
            results.push_back(input_value);
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
        Logger::error("Failed to get user inputs: " + std::string(e.what()));
    }

    return results;
}

std::vector<std::pair<std::string, std::string>> DatabaseManager::getAllUserInputs()
{
    Logger::debug("getAllUserInputs called");
    std::vector<std::pair<std::string, std::string>> results;

    if (!waitForQueueInitialization())
    {
        Logger::error("Access queue not initialized after retries");
        return results;
    }

    // Enqueue the read operation
    auto future = access_queue_->enqueueRead([](DatabaseManager &dbMan)
                                             {
        Logger::debug("Executing getAllUserInputs in access queue");
        
        if (!dbMan.db_)
        {
            Logger::error("Database not initialized");
            return std::any(std::vector<std::pair<std::string, std::string>>());
        }
        
        std::vector<std::pair<std::string, std::string>> results;
        const std::string select_sql = R"(
            SELECT input_type, input_value FROM user_inputs ORDER BY created_at DESC
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
            std::string input_type = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
            std::string input_value = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
            results.emplace_back(input_type, input_value);
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
        Logger::error("Failed to get all user inputs: " + std::string(e.what()));
    }

    return results;
}

DBOpResult DatabaseManager::clearAllUserInputs()
{
    Logger::debug("clearAllUserInputs called");

    if (!waitForQueueInitialization())
    {
        std::string msg = "Access queue not initialized after retries";
        Logger::error(msg);
        return DBOpResult(false, msg);
    }

    // Enqueue the write operation
    access_queue_->enqueueWrite([](DatabaseManager &dbMan)
                                {
        Logger::debug("Executing clearAllUserInputs in write queue");
        
        if (!dbMan.db_)
        {
            std::string error_msg = "Database not initialized";
            Logger::error(error_msg);
            return WriteOperationResult::Failure(error_msg);
        }
        
        const std::string delete_sql = "DELETE FROM user_inputs";

        int rc = sqlite3_exec(dbMan.db_, delete_sql.c_str(), nullptr, nullptr, nullptr);
        if (rc != SQLITE_OK)
        {
            std::string error_msg = "Failed to clear user inputs: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            return WriteOperationResult::Failure(error_msg);
        }

        Logger::debug("All user inputs cleared successfully");
        return WriteOperationResult(true); });

    return DBOpResult(true);
}

bool DatabaseManager::waitForQueueInitialization(int max_retries, int retry_delay_ms)
{
    static std::mutex queue_check_mutex;
    static std::atomic<bool> queue_initialized{false};
    static std::atomic<int> initialization_attempts{0};

    // Use double-checked locking pattern for thread safety
    if (queue_initialized.load())
    {
        return true;
    }

    std::lock_guard<std::mutex> lock(queue_check_mutex);

    // Check again after acquiring lock
    if (queue_initialized.load())
    {
        return true;
    }

    // Check if queue is already initialized
    if (access_queue_)
    {
        queue_initialized.store(true);
        return true;
    }

    // Wait for queue initialization with retries
    for (int attempt = 0; attempt < max_retries; ++attempt)
    {
        initialization_attempts.fetch_add(1);
        Logger::debug("Queue initialization attempt " + std::to_string(attempt + 1) + "/" + std::to_string(max_retries));

        // Check if queue has been initialized
        if (access_queue_)
        {
            queue_initialized.store(true);
            Logger::info("Queue initialized successfully after " + std::to_string(attempt + 1) + " attempts");
            return true;
        }

        // Wait before next attempt (except on last attempt)
        if (attempt < max_retries - 1)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(retry_delay_ms));
        }
    }

    Logger::error("Queue initialization failed after " + std::to_string(max_retries) + " attempts");
    return false;
}

bool DatabaseManager::isValid()
{
    if (!waitForQueueInitialization())
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
    if (!waitForQueueInitialization())
    {
        std::string msg = "Access queue not initialized after retries";
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
    if (!waitForQueueInitialization())
    {
        Logger::error("Access queue not initialized after retries");
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
    if (!waitForQueueInitialization())
    {
        Logger::error("Access queue not initialized after retries");
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

std::vector<std::pair<std::string, std::string>> DatabaseManager::getFilesNeedingProcessingAnyMode()
{
    Logger::debug("getFilesNeedingProcessingAnyMode called");
    std::vector<std::pair<std::string, std::string>> results;
    if (!waitForQueueInitialization())
    {
        Logger::error("Access queue not initialized after retries");
        return results;
    }

    // Enqueue the read operation
    auto future = access_queue_->enqueueRead([](DatabaseManager &dbMan)
                                             {
        Logger::debug("Executing getFilesNeedingProcessingAnyMode in access queue");
        
        if (!dbMan.db_)
        {
            Logger::error("Database not initialized");
            return std::any(std::vector<std::pair<std::string, std::string>>());
        }
        
        std::vector<std::pair<std::string, std::string>> results;
        const std::string select_sql = R"(
            SELECT DISTINCT sf.file_path, sf.file_name 
            FROM scanned_files sf
            WHERE sf.file_metadata IS NULL 
               OR (sf.file_metadata IS NOT NULL AND (
                   NOT EXISTS (SELECT 1 FROM media_processing_results mpr WHERE mpr.file_path = sf.file_path AND mpr.processing_mode = 'FAST')
                   OR NOT EXISTS (SELECT 1 FROM media_processing_results mpr WHERE mpr.file_path = sf.file_path AND mpr.processing_mode = 'BALANCED')
                   OR NOT EXISTS (SELECT 1 FROM media_processing_results mpr WHERE mpr.file_path = sf.file_path AND mpr.processing_mode = 'QUALITY')
               ))
            ORDER BY sf.created_at DESC
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
        Logger::debug("Database read operation completed successfully");
        return std::any(results); });

    try
    {
        auto result = future.get();
        if (result.has_value())
        {
            results = std::any_cast<std::vector<std::pair<std::string, std::string>>>(result);
        }
    }
    catch (const std::exception &e)
    {
        Logger::error("Error getting files needing processing for any mode: " + std::string(e.what()));
    }

    return results;
}

bool DatabaseManager::fileNeedsProcessingForMode(const std::string &file_path, DedupMode mode)
{
    Logger::debug("fileNeedsProcessingForMode called for file: " + file_path + ", mode: " + DedupModes::getModeName(mode));

    if (!waitForQueueInitialization())
    {
        Logger::error("Access queue not initialized after retries");
        return false;
    }

    // Capture parameters for async execution
    std::string captured_file_path = file_path;
    std::string captured_mode = DedupModes::getModeName(mode);
    bool needs_processing = false;

    // Enqueue the read operation
    auto future = access_queue_->enqueueRead([captured_file_path, captured_mode, &needs_processing](DatabaseManager &dbMan)
                                             {
        Logger::debug("Executing fileNeedsProcessingForMode in access queue for: " + captured_file_path + ", mode: " + captured_mode);
        
        if (!dbMan.db_)
        {
            Logger::error("Database not initialized");
            return std::any(false);
        }
        
        // First check if file exists in scanned_files
        const std::string check_sql = "SELECT file_metadata FROM scanned_files WHERE file_path = ?";
        sqlite3_stmt *check_stmt;
        int rc = sqlite3_prepare_v2(dbMan.db_, check_sql.c_str(), -1, &check_stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            Logger::error("Failed to prepare check statement: " + std::string(sqlite3_errmsg(dbMan.db_)));
            return std::any(false);
        }
        
        sqlite3_bind_text(check_stmt, 1, captured_file_path.c_str(), -1, SQLITE_STATIC);
        rc = sqlite3_step(check_stmt);
        if (rc != SQLITE_ROW)
        {
            // File not in scanned_files
            sqlite3_finalize(check_stmt);
            return std::any(false);
        }
        
        // Check if metadata exists
        if (sqlite3_column_type(check_stmt, 0) == SQLITE_NULL)
        {
            // No metadata, needs processing
            sqlite3_finalize(check_stmt);
            needs_processing = true;
            return std::any(needs_processing);
        }
        
        sqlite3_finalize(check_stmt);
        
        // Check if processing result exists for this mode
        const std::string result_sql = "SELECT 1 FROM media_processing_results WHERE file_path = ? AND processing_mode = ?";
        sqlite3_stmt *result_stmt;
        rc = sqlite3_prepare_v2(dbMan.db_, result_sql.c_str(), -1, &result_stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            Logger::error("Failed to prepare result statement: " + std::string(sqlite3_errmsg(dbMan.db_)));
            return std::any(false);
        }
        
        sqlite3_bind_text(result_stmt, 1, captured_file_path.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(result_stmt, 2, captured_mode.c_str(), -1, SQLITE_STATIC);
        rc = sqlite3_step(result_stmt);
        
        bool has_result = (rc == SQLITE_ROW);
        sqlite3_finalize(result_stmt);
        
        return std::any(!has_result); });

    try
    {
        auto result = future.get();
        if (result.has_value())
        {
            needs_processing = std::any_cast<bool>(result);
        }
    }
    catch (const std::exception &e)
    {
        Logger::error("Error checking if file needs processing for mode: " + std::string(e.what()));
    }

    return needs_processing;
}

// Cache map (transcoding) methods

DBOpResult DatabaseManager::insertTranscodingFile(const std::string &source_file_path)
{
    Logger::debug("insertTranscodingFile called for: " + source_file_path);

    if (!waitForQueueInitialization())
    {
        std::string msg = "Access queue not initialized after retries";
        Logger::error(msg);
        return DBOpResult(false, msg);
    }

    // Capture parameters for async execution
    std::string captured_source_file_path = source_file_path;
    std::string error_msg;
    bool success = true;

    // Enqueue the write operation
    access_queue_->enqueueWrite([captured_source_file_path, &error_msg, &success](DatabaseManager &dbMan)
                                {
        Logger::debug("Executing insertTranscodingFile in write queue for: " + captured_source_file_path);
        
        if (!dbMan.db_)
        {
            error_msg = "Database not initialized";
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }
        
        const std::string insert_sql = R"(
            INSERT OR IGNORE INTO cache_map 
            (source_file_path, transcoded_file_path)
            VALUES (?, NULL)
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
        sqlite3_bind_text(stmt, 1, captured_source_file_path.c_str(), -1, SQLITE_STATIC);

        // Execute the statement
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE && rc != SQLITE_OK)
        {
            error_msg = "Failed to insert transcoding file: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }

        Logger::debug("Successfully inserted transcoding file: " + captured_source_file_path);
        return WriteOperationResult(); });

    waitForWrites();
    if (!success)
        return DBOpResult(false, error_msg);
    return DBOpResult(true);
}

DBOpResult DatabaseManager::updateTranscodedFilePath(const std::string &source_file_path, const std::string &transcoded_file_path)
{
    Logger::debug("updateTranscodedFilePath called for: " + source_file_path + " -> " + transcoded_file_path);

    if (!waitForQueueInitialization())
    {
        std::string msg = "Access queue not initialized after retries";
        Logger::error(msg);
        return DBOpResult(false, msg);
    }

    // Capture parameters for async execution
    std::string captured_source_file_path = source_file_path;
    std::string captured_transcoded_file_path = transcoded_file_path;
    std::string error_msg;
    bool success = true;

    // Enqueue the write operation
    access_queue_->enqueueWrite([captured_source_file_path, captured_transcoded_file_path, &error_msg, &success](DatabaseManager &dbMan)
                                {
        Logger::debug("Executing updateTranscodedFilePath in write queue for: " + captured_source_file_path);
        
        if (!dbMan.db_)
        {
            error_msg = "Database not initialized";
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }
        
        const std::string update_sql = R"(
            UPDATE cache_map 
            SET transcoded_file_path = ?, updated_at = CURRENT_TIMESTAMP
            WHERE source_file_path = ?
        )";

        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare_v2(dbMan.db_, update_sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            error_msg = "Failed to prepare statement: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }

        // Bind parameters
        sqlite3_bind_text(stmt, 1, captured_transcoded_file_path.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, captured_source_file_path.c_str(), -1, SQLITE_STATIC);

        // Execute the statement
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE)
        {
            error_msg = "Failed to update transcoded file path: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }

        Logger::debug("Successfully updated transcoded file path: " + captured_source_file_path);
        return WriteOperationResult(); });

    waitForWrites();
    if (!success)
        return DBOpResult(false, error_msg);
    return DBOpResult(true);
}

std::string DatabaseManager::getTranscodedFilePath(const std::string &source_file_path)
{
    Logger::debug("getTranscodedFilePath called for: " + source_file_path);

    if (!waitForQueueInitialization())
    {
        Logger::error("Access queue not initialized after retries");
        return "";
    }

    // Capture parameters for async execution
    std::string captured_source_file_path = source_file_path;
    std::string transcoded_file_path = "";

    // Enqueue the read operation
    auto future = access_queue_->enqueueRead([captured_source_file_path, &transcoded_file_path](DatabaseManager &dbMan)
                                             {
        Logger::debug("Executing getTranscodedFilePath in access queue for: " + captured_source_file_path);
        
        if (!dbMan.db_)
        {
            Logger::error("Database not initialized");
            return std::any(std::string(""));
        }
        
        const std::string select_sql = "SELECT transcoded_file_path FROM cache_map WHERE source_file_path = ?";
        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare_v2(dbMan.db_, select_sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            Logger::error("Failed to prepare select statement: " + std::string(sqlite3_errmsg(dbMan.db_)));
            return std::any(std::string(""));
        }
        
        sqlite3_bind_text(stmt, 1, captured_source_file_path.c_str(), -1, SQLITE_STATIC);
        rc = sqlite3_step(stmt);
        
        if (rc == SQLITE_ROW)
        {
            if (sqlite3_column_type(stmt, 0) != SQLITE_NULL)
            {
                transcoded_file_path = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
            }
        }
        
        sqlite3_finalize(stmt);
        return std::any(transcoded_file_path); });

    try
    {
        auto result = future.get();
        if (result.has_value())
        {
            transcoded_file_path = std::any_cast<std::string>(result);
        }
    }
    catch (const std::exception &e)
    {
        Logger::error("Error getting transcoded file path: " + std::string(e.what()));
    }

    return transcoded_file_path;
}

std::vector<std::string> DatabaseManager::getFilesNeedingTranscoding()
{
    Logger::debug("getFilesNeedingTranscoding called");

    if (!waitForQueueInitialization())
    {
        Logger::error("Access queue not initialized after retries");
        return std::vector<std::string>();
    }

    std::vector<std::string> files_needing_transcoding;

    // Enqueue the read operation
    auto future = access_queue_->enqueueRead([&files_needing_transcoding](DatabaseManager &dbMan)
                                             {
        Logger::debug("Executing getFilesNeedingTranscoding in access queue");
        
        if (!dbMan.db_)
        {
            Logger::error("Database not initialized");
            return std::any(std::vector<std::string>());
        }
        
        const std::string select_sql = "SELECT source_file_path FROM cache_map WHERE transcoded_file_path IS NULL";
        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare_v2(dbMan.db_, select_sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            Logger::error("Failed to prepare select statement: " + std::string(sqlite3_errmsg(dbMan.db_)));
            return std::any(std::vector<std::string>());
        }
        
        while (sqlite3_step(stmt) == SQLITE_ROW)
        {
            std::string source_file_path = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
            files_needing_transcoding.push_back(source_file_path);
        }
        
        sqlite3_finalize(stmt);
        return std::any(files_needing_transcoding); });

    try
    {
        auto result = future.get();
        if (result.has_value())
        {
            files_needing_transcoding = std::any_cast<std::vector<std::string>>(result);
        }
    }
    catch (const std::exception &e)
    {
        Logger::error("Error getting files needing transcoding: " + std::string(e.what()));
    }

    return files_needing_transcoding;
}

bool DatabaseManager::fileNeedsTranscoding(const std::string &source_file_path)
{
    Logger::debug("fileNeedsTranscoding called for: " + source_file_path);

    if (!waitForQueueInitialization())
    {
        Logger::error("Access queue not initialized after retries");
        return false;
    }

    // Capture parameters for async execution
    std::string captured_source_file_path = source_file_path;
    bool needs_transcoding = false;

    // Enqueue the read operation
    auto future = access_queue_->enqueueRead([captured_source_file_path, &needs_transcoding](DatabaseManager &dbMan)
                                             {
        Logger::debug("Executing fileNeedsTranscoding in access queue for: " + captured_source_file_path);
        
        if (!dbMan.db_)
        {
            Logger::error("Database not initialized");
            return std::any(false);
        }
        
        const std::string select_sql = "SELECT 1 FROM cache_map WHERE source_file_path = ? AND transcoded_file_path IS NULL";
        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare_v2(dbMan.db_, select_sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            Logger::error("Failed to prepare select statement: " + std::string(sqlite3_errmsg(dbMan.db_)));
            return std::any(false);
        }
        
        sqlite3_bind_text(stmt, 1, captured_source_file_path.c_str(), -1, SQLITE_STATIC);
        rc = sqlite3_step(stmt);
        
        needs_transcoding = (rc == SQLITE_ROW);
        sqlite3_finalize(stmt);
        
        return std::any(needs_transcoding); });

    try
    {
        auto result = future.get();
        if (result.has_value())
        {
            needs_transcoding = std::any_cast<bool>(result);
        }
    }
    catch (const std::exception &e)
    {
        Logger::error("Error checking if file needs transcoding: " + std::string(e.what()));
    }

    return needs_transcoding;
}

DBOpResult DatabaseManager::removeTranscodingRecord(const std::string &source_file_path)
{
    Logger::debug("removeTranscodingRecord called for: " + source_file_path);

    if (!waitForQueueInitialization())
    {
        std::string msg = "Access queue not initialized after retries";
        Logger::error(msg);
        return DBOpResult(false, msg);
    }

    // Capture parameters for async execution
    std::string captured_source_file_path = source_file_path;
    std::string error_msg;
    bool success = true;

    // Enqueue the write operation
    access_queue_->enqueueWrite([captured_source_file_path, &error_msg, &success](DatabaseManager &dbMan)
                                {
        Logger::debug("Executing removeTranscodingRecord in write queue for: " + captured_source_file_path);
        
        if (!dbMan.db_)
        {
            error_msg = "Database not initialized";
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }
        
        const std::string delete_sql = "DELETE FROM cache_map WHERE source_file_path = ?";

        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare_v2(dbMan.db_, delete_sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            error_msg = "Failed to prepare statement: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }

        // Bind parameters
        sqlite3_bind_text(stmt, 1, captured_source_file_path.c_str(), -1, SQLITE_STATIC);

        // Execute the statement
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE)
        {
            error_msg = "Failed to remove transcoding record: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }

        Logger::debug("Successfully removed transcoding record: " + captured_source_file_path);
        return WriteOperationResult(); });

    waitForWrites();
    if (!success)
        return DBOpResult(false, error_msg);
    return DBOpResult(true);
}

DBOpResult DatabaseManager::clearAllTranscodingRecords()
{
    Logger::debug("clearAllTranscodingRecords called");

    if (!waitForQueueInitialization())
    {
        std::string msg = "Access queue not initialized after retries";
        Logger::error(msg);
        return DBOpResult(false, msg);
    }

    std::string error_msg;
    bool success = true;

    // Enqueue the write operation
    access_queue_->enqueueWrite([&error_msg, &success](DatabaseManager &dbMan)
                                {
        Logger::debug("Executing clearAllTranscodingRecords in write queue");
        
        if (!dbMan.db_)
        {
            error_msg = "Database not initialized";
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }
        
        const std::string delete_sql = "DELETE FROM cache_map";

        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare_v2(dbMan.db_, delete_sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            error_msg = "Failed to prepare statement: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }

        // Execute the statement
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE)
        {
            error_msg = "Failed to clear all transcoding records: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }

        Logger::debug("Successfully cleared all transcoding records");
        return WriteOperationResult(); });

    waitForWrites();
    if (!success)
        return DBOpResult(false, error_msg);
    return DBOpResult(true);
}

// Update the metadata for a file after processing
DBOpResult DatabaseManager::updateFileMetadata(const std::string &file_path, const std::string &metadata_str)
{
    if (!waitForQueueInitialization())
    {
        std::string msg = "Access queue not initialized after retries";
        Logger::error(msg);
        return DBOpResult(false, msg);
    }

    // Capture parameters for async execution
    std::string captured_file_path = file_path;
    std::string captured_metadata_str = metadata_str;
    std::string error_msg;
    bool success = true;

    // Enqueue the write operation
    access_queue_->enqueueWrite([captured_file_path, captured_metadata_str, &error_msg, &success](DatabaseManager &dbMan)
                                {
        Logger::debug("Executing updateFileMetadata in write queue for: " + captured_file_path);
        
        const std::string update_sql = "UPDATE scanned_files SET file_metadata = ? WHERE file_path = ?";
        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare_v2(dbMan.db_, update_sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            error_msg = "Failed to prepare update statement: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }

        sqlite3_bind_text(stmt, 1, captured_metadata_str.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, captured_file_path.c_str(), -1, SQLITE_STATIC);

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE)
        {
            error_msg = "Failed to update file metadata: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }

        Logger::debug("Updated file metadata for: " + captured_file_path);
        return WriteOperationResult(); });

    waitForWrites();
    if (!success)
        return DBOpResult(false, error_msg);
    return DBOpResult(true, "");
}

std::pair<DBOpResult, size_t> DatabaseManager::updateFileMetadataWithId(const std::string &file_path, const std::string &metadata_str)
{
    if (!waitForQueueInitialization())
    {
        std::string msg = "Access queue not initialized after retries";
        Logger::error(msg);
        return {DBOpResult(false, msg), 0};
    }

    // Capture parameters for async execution
    std::string captured_file_path = file_path;
    std::string captured_metadata_str = metadata_str;
    std::string error_msg;
    bool success = true;

    // Enqueue the write operation and get the operation ID
    size_t operation_id = access_queue_->enqueueWrite([captured_file_path, captured_metadata_str, &error_msg, &success](DatabaseManager &dbMan)
                                                      {
        Logger::debug("Executing updateFileMetadata in write queue for: " + captured_file_path);
        
        const std::string update_sql = "UPDATE scanned_files SET file_metadata = ? WHERE file_path = ?";
        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare_v2(dbMan.db_, update_sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            error_msg = "Failed to prepare update statement: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }

        sqlite3_bind_text(stmt, 1, captured_metadata_str.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, captured_file_path.c_str(), -1, SQLITE_STATIC);

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE)
        {
            error_msg = "Failed to update file metadata: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }

        Logger::debug("Updated file metadata for: " + captured_file_path);
        return WriteOperationResult(); });

    waitForWrites();
    if (!success)
        return {DBOpResult(false, error_msg), operation_id};
    return {DBOpResult(true, ""), operation_id};
}

// Set processing flag for a specific mode after successful processing
DBOpResult DatabaseManager::setProcessingFlag(const std::string &file_path, DedupMode mode)
{
    if (!waitForQueueInitialization())
    {
        std::string msg = "Access queue not initialized after retries";
        Logger::error(msg);
        return DBOpResult(false, msg);
    }

    // Capture parameters for async execution
    std::string captured_file_path = file_path;
    DedupMode captured_mode = mode;
    std::string error_msg;
    bool success = true;

    // Enqueue the write operation
    access_queue_->enqueueWrite([captured_file_path, captured_mode, &error_msg, &success](DatabaseManager &dbMan)
                                {
        Logger::debug("Executing setProcessingFlag in write queue for: " + captured_file_path + " mode: " + DedupModes::getModeName(captured_mode));
        
        if (!dbMan.db_)
        {
            error_msg = "Database not initialized";
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }
        
        // Build the SQL query based on the mode
        std::string update_sql;
        switch (captured_mode)
        {
            case DedupMode::FAST:
                update_sql = "UPDATE scanned_files SET processed_fast = 1 WHERE file_path = ?";
                break;
            case DedupMode::BALANCED:
                update_sql = "UPDATE scanned_files SET processed_balanced = 1 WHERE file_path = ?";
                break;
            case DedupMode::QUALITY:
                update_sql = "UPDATE scanned_files SET processed_quality = 1 WHERE file_path = ?";
                break;
            default:
                error_msg = "Unknown processing mode: " + DedupModes::getModeName(captured_mode);
                Logger::error(error_msg);
                success = false;
                return WriteOperationResult::Failure(error_msg);
        }
        
        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare_v2(dbMan.db_, update_sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            error_msg = "Failed to prepare update statement: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }

        sqlite3_bind_text(stmt, 1, captured_file_path.c_str(), -1, SQLITE_STATIC);

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE)
        {
            error_msg = "Failed to set processing flag: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }

        Logger::debug("Set processing flag for: " + captured_file_path + " mode: " + DedupModes::getModeName(captured_mode));
        return WriteOperationResult(); });

    waitForWrites();
    if (!success)
        return DBOpResult(false, error_msg);
    return DBOpResult(true);
}

bool DatabaseManager::tryAcquireProcessingLock(const std::string &file_path, DedupMode mode)
{
    if (!waitForQueueInitialization())
    {
        Logger::error("Access queue not initialized after retries");
        return false;
    }

    // Capture parameters for async execution
    std::string captured_file_path = file_path;
    DedupMode captured_mode = mode;
    std::atomic<bool> lock_acquired{false};
    std::string error_msg;

    // Enqueue the write operation
    access_queue_->enqueueWrite([captured_file_path, captured_mode, &lock_acquired, &error_msg](DatabaseManager &dbMan)
                                {
        Logger::debug("Executing tryAcquireProcessingLockAtomic in write queue for: " + captured_file_path + " mode: " + DedupModes::getModeName(captured_mode));
        
        if (!dbMan.db_)
        {
            error_msg = "Database not initialized";
            Logger::error(error_msg);
            lock_acquired.store(false);
            return WriteOperationResult::Failure(error_msg);
        }
        
        // Build the SQL query based on the mode - only update if not already processed
        std::string update_sql;
        switch (captured_mode)
        {
            case DedupMode::FAST:
                update_sql = "UPDATE scanned_files SET processed_fast = 1 WHERE file_path = ? AND processed_fast = 0";
                break;
            case DedupMode::BALANCED:
                update_sql = "UPDATE scanned_files SET processed_balanced = 1 WHERE file_path = ? AND processed_balanced = 0";
                break;
            case DedupMode::QUALITY:
                update_sql = "UPDATE scanned_files SET processed_quality = 1 WHERE file_path = ? AND processed_quality = 0";
                break;
            default:
                error_msg = "Unknown processing mode: " + DedupModes::getModeName(captured_mode);
                Logger::error(error_msg);
                lock_acquired.store(false);
                return WriteOperationResult::Failure(error_msg);
        }
        
        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare_v2(dbMan.db_, update_sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            error_msg = "Failed to prepare atomic update statement: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            lock_acquired.store(false);
            return WriteOperationResult::Failure(error_msg);
        }

        sqlite3_bind_text(stmt, 1, captured_file_path.c_str(), -1, SQLITE_STATIC);

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE)
        {
            error_msg = "Failed to execute atomic update: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            lock_acquired.store(false);
            return WriteOperationResult::Failure(error_msg);
        }

        // Check if any rows were actually updated
        int rows_affected = sqlite3_changes(dbMan.db_);
        if (rows_affected > 0)
        {
            Logger::debug("Atomically acquired processing lock for " + captured_file_path + " mode " + DedupModes::getModeName(captured_mode));
            lock_acquired.store(true);
        }
        else
        {
            Logger::debug("File " + captured_file_path + " already processed for mode " + DedupModes::getModeName(captured_mode));
            lock_acquired.store(false);
        }

        return WriteOperationResult(); });

    waitForWrites();
    return lock_acquired.load();
}