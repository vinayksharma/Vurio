#include "database/database_manager.hpp"
#include "database/database_access_queue.hpp"
#include "core/server_config_manager.hpp"
#include "core/duplicate_linker.hpp"
#include "core/dedup_modes.hpp"
#include "core/media_processor.hpp"
#include "core/file_utils.hpp"
#include "core/mount_manager.hpp"
#include "logging/logger.hpp"
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <filesystem>
#include <thread>
#include <mutex>
#include <chrono>
#include <atomic>
#include <unordered_set>
#include <any>
#include <functional>
#include <openssl/sha.h>
#include <unistd.h>

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
    // In test mode, allow reinitializing with a different database path
    else if (isTestMode() && !db_path.empty() && instance_->db_path_ != db_path)
    {
        Logger::info("Test mode: Reinitializing DatabaseManager with new test database: " + db_path);
        instance_->waitForWrites(); // Ensure all writes complete
        instance_.reset();
        instance_ = std::unique_ptr<DatabaseManager>(new DatabaseManager(db_path));
    }
    // After the singleton is initialized, allow calls with an empty db_path
    // without emitting warnings. Only warn if a non-empty, different path is provided.
    else if (!db_path.empty() && instance_->db_path_ != db_path)
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

bool DatabaseManager::isTestMode()
{
    const char *test_mode = std::getenv("TEST_MODE");
    return test_mode != nullptr && std::string(test_mode) == "1";
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
    if (!createFlagsTable())
        Logger::error("Failed to create flags table");
    if (!createScannedFilesChangeTriggers())
        Logger::error("Failed to create scanned_files change triggers");
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
            links_fast TEXT,              -- Comma-separated list of duplicate file IDs found in FAST mode
            links_balanced TEXT,          -- Comma-separated list of duplicate file IDs found in BALANCED mode
            links_quality TEXT,           -- Comma-separated list of duplicate file IDs found in QUALITY mode
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

bool DatabaseManager::createFlagsTable()
{
    const std::string sql = R"(
        CREATE TABLE IF NOT EXISTS flags (
            name TEXT PRIMARY KEY,
            value TEXT NOT NULL DEFAULT '0',
            updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        )
    )";
    return executeStatement(sql).success;
}

bool DatabaseManager::createScannedFilesChangeTriggers()
{
    // Create a trigger to set transcode_preprocess_scanned_files_changed to 1 on INSERT, UPDATE, DELETE
    const std::string sql = R"(
        CREATE TRIGGER IF NOT EXISTS trg_scanned_files_changed_insert
        AFTER INSERT ON scanned_files
        BEGIN
            INSERT INTO flags(name, value, updated_at) VALUES ('transcode_preprocess_scanned_files_changed', 1, CURRENT_TIMESTAMP)
            ON CONFLICT(name) DO UPDATE SET value = 1, updated_at = CURRENT_TIMESTAMP;
        END;
    )";
    auto res1 = executeStatement(sql);
    if (!res1.success)
        return false;

    const std::string sql2 = R"(
        CREATE TRIGGER IF NOT EXISTS trg_scanned_files_changed_update
        AFTER UPDATE ON scanned_files
        BEGIN
            INSERT INTO flags(name, value, updated_at) VALUES ('transcode_preprocess_scanned_files_changed', 1, CURRENT_TIMESTAMP)
            ON CONFLICT(name) DO UPDATE SET value = 1, updated_at = CURRENT_TIMESTAMP;
        END;
    )";
    auto res2 = executeStatement(sql2);
    if (!res2.success)
        return false;

    const std::string sql3 = R"(
        CREATE TRIGGER IF NOT EXISTS trg_scanned_files_changed_delete
        AFTER DELETE ON scanned_files
        BEGIN
            INSERT INTO flags(name, value, updated_at) VALUES ('transcode_preprocess_scanned_files_changed', 1, CURRENT_TIMESTAMP)
            ON CONFLICT(name) DO UPDATE SET value = 1, updated_at = CURRENT_TIMESTAMP;
        END;
    )";
    auto res3 = executeStatement(sql3);
    if (!res3.success)
        return false;

    return true;
}

bool DatabaseManager::getFlag(const std::string &flag_name)
{
    if (!waitForQueueInitialization())
        return false;

    bool value = false;
    auto future = access_queue_->enqueueRead([&flag_name, &value](DatabaseManager &dbMan)
                                             {
        if (!dbMan.db_) return std::any(false);
        const std::string select_sql = "SELECT value FROM flags WHERE name = ?";
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(dbMan.db_, select_sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
            return std::any(false);
        sqlite3_bind_text(stmt, 1, flag_name.c_str(), -1, SQLITE_TRANSIENT);
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW)
        {
            value = sqlite3_column_int(stmt, 0) != 0;
        }
        sqlite3_finalize(stmt);
        return std::any(value); });
    try
    {
        return std::any_cast<bool>(future.get());
    }
    catch (...)
    {
        return false;
    }
}

DBOpResult DatabaseManager::setFlag(const std::string &flag_name, bool flag_value)
{
    if (!waitForQueueInitialization())
        return DBOpResult(false, "queue not initialized");

    std::string captured_name = flag_name;
    int captured_value = flag_value ? 1 : 0;
    std::string error_msg;
    bool success = true;

    access_queue_->enqueueWrite([captured_name, captured_value, &error_msg, &success](DatabaseManager &dbMan)
                                {
        if (!dbMan.db_)
        {
            error_msg = "Database not initialized";
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }
        const std::string upsert_sql = R"(
            INSERT INTO flags(name, value, updated_at) VALUES(?, ?, CURRENT_TIMESTAMP)
            ON CONFLICT(name) DO UPDATE SET value = excluded.value, updated_at = CURRENT_TIMESTAMP
        )";
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(dbMan.db_, upsert_sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        {
            error_msg = std::string("Failed to prepare statement: ") + sqlite3_errmsg(dbMan.db_);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }
        sqlite3_bind_text(stmt, 1, captured_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, captured_value);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE && rc != SQLITE_OK)
        {
            error_msg = std::string("Failed to set flag: ") + sqlite3_errmsg(dbMan.db_);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }
        return WriteOperationResult(); });

    waitForWrites();
    if (!success)
        return DBOpResult(false, error_msg);
    return DBOpResult(true);
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
            (file_path, processing_mode, success, 
             artifact_format, artifact_hash, artifact_confidence, 
             artifact_metadata, artifact_data)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?)
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

        if (captured_result.artifact.format.empty())
        {
            sqlite3_bind_null(stmt, 4);
        }
        else
        {
            sqlite3_bind_text(stmt, 4, captured_result.artifact.format.c_str(), -1, SQLITE_STATIC);
        }

        if (captured_result.artifact.hash.empty())
        {
            sqlite3_bind_null(stmt, 5);
        }
        else
        {
            sqlite3_bind_text(stmt, 5, captured_result.artifact.hash.c_str(), -1, SQLITE_STATIC);
        }

        sqlite3_bind_double(stmt, 6, captured_result.artifact.confidence);

        if (captured_result.artifact.metadata.empty())
        {
            sqlite3_bind_null(stmt, 7);
        }
        else
        {
            sqlite3_bind_text(stmt, 7, captured_result.artifact.metadata.c_str(), -1, SQLITE_STATIC);
        }

        if (captured_result.artifact.data.empty())
        {
            sqlite3_bind_blob(stmt, 8, nullptr, 0, SQLITE_STATIC);
        }
        else
        {
            sqlite3_bind_blob(stmt, 8, captured_result.artifact.data.data(),
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
            (file_path, processing_mode, success, 
             artifact_format, artifact_hash, artifact_confidence, 
             artifact_metadata, artifact_data)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?)
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

        if (captured_result.artifact.format.empty())
        {
            sqlite3_bind_null(stmt, 4);
        }
        else
        {
            sqlite3_bind_text(stmt, 4, captured_result.artifact.format.c_str(), -1, SQLITE_STATIC);
        }

        if (captured_result.artifact.hash.empty())
        {
            sqlite3_bind_null(stmt, 5);
        }
        else
        {
            sqlite3_bind_text(stmt, 5, captured_result.artifact.hash.c_str(), -1, SQLITE_STATIC);
        }

        sqlite3_bind_double(stmt, 6, captured_result.artifact.confidence);

        if (captured_result.artifact.metadata.empty())
        {
            sqlite3_bind_null(stmt, 7);
        }
        else
        {
            sqlite3_bind_text(stmt, 7, captured_result.artifact.metadata.c_str(), -1, SQLITE_STATIC);
        }

        if (captured_result.artifact.data.empty())
        {
            sqlite3_bind_blob(stmt, 8, nullptr, 0, SQLITE_STATIC);
        }
        else
        {
            sqlite3_bind_blob(stmt, 8, captured_result.artifact.data.data(),
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
            SELECT processing_mode, success, 
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
                result.artifact.format = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
            }

            if (sqlite3_column_type(stmt, 3) != SQLITE_NULL)
            {
                result.artifact.hash = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));
            }

            result.artifact.confidence = sqlite3_column_double(stmt, 4);

            if (sqlite3_column_type(stmt, 5) != SQLITE_NULL)
            {
                result.artifact.metadata = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 5));
            }

            if (sqlite3_column_type(stmt, 6) != SQLITE_NULL)
            {
                const void *blob_data = sqlite3_column_blob(stmt, 6);
                int blob_size = sqlite3_column_bytes(stmt, 6);
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
            SELECT file_path, processing_mode, success, 
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
            // Request DuplicateLinker to do a full rescan since existing links may be stale
            DuplicateLinker::getInstance().requestFullRescan();
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
            // New files arriving can introduce new duplicates; request a full rescan pass
            DuplicateLinker::getInstance().requestFullRescan();
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
    auto future = access_queue_->enqueueRead([captured_mode, this](DatabaseManager &dbMan)
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
        std::string file_type_clauses = generateFileTypeLikeClauses();
        
        switch (captured_mode)
        {
            case DedupMode::FAST:
                select_sql = "SELECT file_path, file_name FROM scanned_files WHERE processed_fast = 0 AND (" + file_type_clauses + ") ORDER BY created_at DESC LIMIT ?";
                break;
            case DedupMode::BALANCED:
                select_sql = "SELECT file_path, file_name FROM scanned_files WHERE processed_balanced = 0 AND (" + file_type_clauses + ") ORDER BY created_at DESC LIMIT ?";
                break;
            case DedupMode::QUALITY:
                select_sql = "SELECT file_path, file_name FROM scanned_files WHERE processed_quality = 0 AND (" + file_type_clauses + ") ORDER BY created_at DESC LIMIT ?";
                break;
        }
        
        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare_v2(dbMan.db_, select_sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            Logger::error("Failed to prepare select statement: " + std::string(sqlite3_errmsg(dbMan.db_)));
            return std::any(results);
        }

        // Bind the limit parameter (use a large number to get all files)
        sqlite3_bind_int(stmt, 1, 1000);

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

int DatabaseManager::getFileId(const std::string &file_path)
{
    if (!waitForQueueInitialization())
        return -1;
    std::string captured = file_path;
    auto future = access_queue_->enqueueRead([captured](DatabaseManager &dbMan)
                                             {
        if (!dbMan.db_)
            return std::any(-1);
        const char *sql = "SELECT id FROM scanned_files WHERE file_path = ?";
        sqlite3_stmt *stmt = nullptr;
        int rc = sqlite3_prepare_v2(dbMan.db_, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
            return std::any(-1);
        sqlite3_bind_text(stmt, 1, captured.c_str(), -1, SQLITE_STATIC);
        int id = -1;
        if (sqlite3_step(stmt) == SQLITE_ROW)
            id = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        return std::any(id); });
    try
    {
        return std::any_cast<int>(future.get());
    }
    catch (...)
    {
        return -1;
    }
}

long DatabaseManager::getMaxProcessingResultId()
{
    if (!waitForQueueInitialization())
        return 0;
    auto future = access_queue_->enqueueRead([](DatabaseManager &dbMan)
                                             {
        if (!dbMan.db_)
            return std::any(0L);
        const char *sql = "SELECT IFNULL(MAX(id),0) FROM media_processing_results";
        sqlite3_stmt *stmt = nullptr;
        long max_id = 0;
        if (sqlite3_prepare_v2(dbMan.db_, sql, -1, &stmt, nullptr) == SQLITE_OK)
        {
            if (sqlite3_step(stmt) == SQLITE_ROW)
            {
                max_id = sqlite3_column_int64(stmt, 0);
            }
            sqlite3_finalize(stmt);
        }
        return std::any(max_id); });
    try
    {
        return std::any_cast<long>(future.get());
    }
    catch (...)
    {
        return 0;
    }
}

std::vector<std::tuple<long, std::string, std::string>>
DatabaseManager::getNewSuccessfulResults(DedupMode mode, long last_seen_id)
{
    std::vector<std::tuple<long, std::string, std::string>> out;
    if (!waitForQueueInitialization())
        return out;
    auto future = access_queue_->enqueueRead([mode, last_seen_id](DatabaseManager &dbMan)
                                             {
        std::vector<std::tuple<long, std::string, std::string>> rows;
        if (!dbMan.db_)
            return std::any(rows);
        const std::string sql =
            "SELECT id, file_path, artifact_hash FROM media_processing_results "
            "WHERE id > ? AND success = 1 AND artifact_hash IS NOT NULL AND processing_mode = ? ORDER BY id";
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(dbMan.db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
            return std::any(rows);
        sqlite3_bind_int64(stmt, 1, last_seen_id);
        std::string mode_name = DedupModes::getModeName(mode);
        sqlite3_bind_text(stmt, 2, mode_name.c_str(), -1, SQLITE_STATIC);
        while (sqlite3_step(stmt) == SQLITE_ROW)
        {
            long id = sqlite3_column_int64(stmt, 0);
            std::string fp = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
            std::string h = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
            rows.emplace_back(id, fp, h);
        }
        sqlite3_finalize(stmt);
        return std::any(rows); });
    try
    {
        out = std::any_cast<std::vector<std::tuple<long, std::string, std::string>>>(future.get());
    }
    catch (...)
    {
    }
    return out;
}

std::vector<std::pair<std::string, std::string>>
DatabaseManager::getSuccessfulFileHashesForMode(DedupMode mode)
{
    std::vector<std::pair<std::string, std::string>> out;
    if (!waitForQueueInitialization())
        return out;
    auto future = access_queue_->enqueueRead([mode](DatabaseManager &dbMan)
                                             {
        std::vector<std::pair<std::string, std::string>> rows;
        if (!dbMan.db_)
            return std::any(rows);
        const std::string sql =
            "SELECT file_path, artifact_hash FROM media_processing_results "
            "WHERE success = 1 AND artifact_hash IS NOT NULL AND processing_mode = ?";
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(dbMan.db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
            return std::any(rows);
        std::string mode_name = DedupModes::getModeName(mode);
        sqlite3_bind_text(stmt, 1, mode_name.c_str(), -1, SQLITE_STATIC);
        while (sqlite3_step(stmt) == SQLITE_ROW)
        {
            std::string fp = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
            std::string h = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
            rows.emplace_back(fp, h);
        }
        sqlite3_finalize(stmt);
        return std::any(rows); });
    try
    {
        out = std::any_cast<std::vector<std::pair<std::string, std::string>>>(future.get());
    }
    catch (...)
    {
    }
    return out;
}

std::vector<std::string>
DatabaseManager::getAllFilePathsForHashAndMode(const std::string &artifact_hash, DedupMode mode)
{
    std::vector<std::string> out;
    if (!waitForQueueInitialization())
        return out;
    std::string captured_hash = artifact_hash;
    auto future = access_queue_->enqueueRead([captured_hash, mode](DatabaseManager &dbMan)
                                             {
        std::vector<std::string> rows;
        if (!dbMan.db_)
            return std::any(rows);
        const std::string sql =
            "SELECT sf.file_path FROM media_processing_results mpr JOIN scanned_files sf ON sf.file_path = mpr.file_path "
            "WHERE mpr.success = 1 AND mpr.artifact_hash = ? AND mpr.processing_mode = ?";
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(dbMan.db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
            return std::any(rows);
        sqlite3_bind_text(stmt, 1, captured_hash.c_str(), -1, SQLITE_STATIC);
        std::string mode_name = DedupModes::getModeName(mode);
        sqlite3_bind_text(stmt, 2, mode_name.c_str(), -1, SQLITE_STATIC);
        while (sqlite3_step(stmt) == SQLITE_ROW)
        {
            std::string fp = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
            rows.push_back(fp);
        }
        sqlite3_finalize(stmt);
        return std::any(rows); });
    try
    {
        out = std::any_cast<std::vector<std::string>>(future.get());
    }
    catch (...)
    {
    }
    return out;
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
    // Get current links for the current server mode
    std::vector<int> current_links = getFileLinksForCurrentMode(file_path);

    // Check if link already exists
    if (std::find(current_links.begin(), current_links.end(), linked_id) != current_links.end())
    {
        Logger::debug("Link already exists for file: " + file_path + " to ID: " + std::to_string(linked_id));
        return DBOpResult(true, ""); // Already linked
    }

    // Add new link
    current_links.push_back(linked_id);

    // Get the current deduplication mode from the server configuration
    try
    {
        auto &config = ServerConfigManager::getInstance();
        DedupMode current_mode = config.getDedupMode();
        return setFileLinksForMode(file_path, current_links, current_mode);
    }
    catch (const std::exception &e)
    {
        Logger::error("Failed to get current deduplication mode: " + std::string(e.what()));
        return DBOpResult(false, "Failed to get current deduplication mode");
    }
}

DBOpResult DatabaseManager::removeFileLink(const std::string &file_path, int linked_id)
{
    // Get current links for the current server mode
    std::vector<int> current_links = getFileLinksForCurrentMode(file_path);

    // Remove the link
    auto it = std::find(current_links.begin(), current_links.end(), linked_id);
    if (it == current_links.end())
    {
        Logger::debug("Link not found for file: " + file_path + " to ID: " + std::to_string(linked_id));
        return DBOpResult(true, ""); // Link doesn't exist
    }

    current_links.erase(it);

    // Get the current deduplication mode from the server configuration
    try
    {
        auto &config = ServerConfigManager::getInstance();
        DedupMode current_mode = config.getDedupMode();
        return setFileLinksForMode(file_path, current_links, current_mode);
    }
    catch (const std::exception &e)
    {
        Logger::error("Failed to get current deduplication mode: " + std::string(e.what()));
        return DBOpResult(false, "Failed to get current deduplication mode");
    }
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
        const std::string select_sql = "SELECT file_path FROM scanned_files WHERE (links_fast LIKE ?) OR (links_balanced LIKE ?) OR (links_quality LIKE ?)";
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
        sqlite3_bind_text(stmt, 2, search_pattern.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, search_pattern.c_str(), -1, SQLITE_STATIC);

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
    auto future = access_queue_->enqueueRead([this](DatabaseManager &dbMan)
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
    DedupMode captured_mode = mode;
    bool needs_processing = false;

    // Enqueue the read operation
    auto future = access_queue_->enqueueRead([captured_file_path, captured_mode, &needs_processing](DatabaseManager &dbMan)
                                             {
        Logger::debug("Executing fileNeedsProcessingForMode in access queue for: " + captured_file_path + ", mode: " + DedupModes::getModeName(captured_mode));
        
        if (!dbMan.db_)
        {
            Logger::error("Database not initialized");
            return std::any(false);
        }
        
        // Check processing flag for this mode
        std::string flag_column;
        switch (captured_mode)
        {
            case DedupMode::FAST:
                flag_column = "processed_fast";
                break;
            case DedupMode::BALANCED:
                flag_column = "processed_balanced";
                break;
            case DedupMode::QUALITY:
                flag_column = "processed_quality";
                break;
            default:
                Logger::error("Unknown processing mode: " + DedupModes::getModeName(captured_mode));
                return std::any(false);
        }
        
        const std::string check_sql = "SELECT " + flag_column + ", file_name FROM scanned_files WHERE file_path = ?";
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
        
        int processing_flag = sqlite3_column_int(check_stmt, 0);
        std::string file_name = reinterpret_cast<const char *>(sqlite3_column_text(check_stmt, 1));
        sqlite3_finalize(check_stmt);
        
        // Check processing flag:
        // 0 = needs processing
        // -1 = in progress (should be processed)
        // 1 = already processed
        needs_processing = (processing_flag == 0 || processing_flag == -1);
        
        // Only return true if the file is actually supported
        if (needs_processing) {
            // Check if file has a supported extension using configuration
            std::string file_extension = MediaProcessor::getFileExtension(captured_file_path);
            auto enabled_types = ServerConfigManager::getInstance().getEnabledFileTypes();
            bool has_supported_extension = std::find(enabled_types.begin(), enabled_types.end(), file_extension) != enabled_types.end();
            
            needs_processing = has_supported_extension;
        }
        
        Logger::debug("File " + captured_file_path + " processing flag for mode " + DedupModes::getModeName(captured_mode) + ": " + std::to_string(processing_flag) + " (needs processing: " + (needs_processing ? "true" : "false") + ")");
        
        return std::any(needs_processing); });

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

std::string DatabaseManager::claimNextTranscodingJob()
{
    Logger::debug("claimNextTranscodingJob called");

    if (!waitForQueueInitialization())
    {
        Logger::error("Access queue not initialized after retries");
        return "";
    }

    std::string file_path;
    auto future = access_queue_->enqueueRead([&file_path](DatabaseManager &dbMan)
                                             {
        Logger::debug("Executing claimNextTranscodingJob in access queue");

        if (!dbMan.db_)
        {
            Logger::error("Database not initialized");
            return std::any(std::string(""));
        }

        const std::string select_sql =
            "SELECT source_file_path FROM cache_map WHERE status = 0 AND transcoded_file_path IS NULL ORDER BY created_at ASC LIMIT 1";
        sqlite3_stmt *stmt = nullptr;
        int rc = sqlite3_prepare_v2(dbMan.db_, select_sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            Logger::error("Failed to prepare job selection statement: " + std::string(sqlite3_errmsg(dbMan.db_)));
            return std::any(std::string(""));
        }

        rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW)
        {
            file_path = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
        }

        sqlite3_finalize(stmt);
        return std::any(file_path); });

    try
    {
        auto result = future.get();
        if (result.has_value())
        {
            file_path = std::any_cast<std::string>(result);
        }
    }
    catch (const std::exception &e)
    {
        Logger::error("Error in claimNextTranscodingJob: " + std::string(e.what()));
    }

    if (!file_path.empty())
    {
        Logger::debug("Claimed next transcoding job: " + file_path);
    }
    return file_path;
}

bool DatabaseManager::markTranscodingJobInProgress(const std::string &source_file_path)
{
    if (!waitForQueueInitialization())
    {
        std::string msg = "Access queue not initialized after retries";
        Logger::error(msg);
        return false;
    }

    std::string captured = source_file_path;
    bool success = true;
    std::string error_msg;
    access_queue_->enqueueWrite([captured, &success, &error_msg](DatabaseManager &dbMan)
                                {
        if (!dbMan.db_)
        {
            error_msg = "Database not initialized";
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }
        const std::string update_sql =
            "UPDATE cache_map SET status = 1, worker_id = ?, updated_at = CURRENT_TIMESTAMP WHERE source_file_path = ?";
        sqlite3_stmt *stmt = nullptr;
        int rc = sqlite3_prepare_v2(dbMan.db_, update_sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            error_msg = "Failed to prepare mark in-progress: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }
        std::string worker_id = std::to_string(getpid());
        sqlite3_bind_text(stmt, 1, worker_id.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, captured.c_str(), -1, SQLITE_STATIC);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE)
        {
            error_msg = "Failed to mark in-progress: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }
        Logger::debug("Marked job in progress: " + captured);
        return WriteOperationResult(); });
    waitForWrites();
    return success;
}

bool DatabaseManager::markTranscodingJobCompleted(const std::string &source_file_path, const std::string &transcoded_file_path)
{
    if (!waitForQueueInitialization())
    {
        std::string msg = "Access queue not initialized after retries";
        Logger::error(msg);
        return false;
    }
    std::string src = source_file_path;
    std::string out = transcoded_file_path;
    bool success = true;
    std::string error_msg;
    access_queue_->enqueueWrite([src, out, &success, &error_msg](DatabaseManager &dbMan)
                                {
        if (!dbMan.db_)
        {
            error_msg = "Database not initialized";
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }
        const std::string update_sql =
            "UPDATE cache_map SET status = 2, transcoded_file_path = ?, updated_at = CURRENT_TIMESTAMP WHERE source_file_path = ?";
        sqlite3_stmt *stmt = nullptr;
        int rc = sqlite3_prepare_v2(dbMan.db_, update_sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            error_msg = "Failed to prepare mark completed: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }
        sqlite3_bind_text(stmt, 1, out.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, src.c_str(), -1, SQLITE_STATIC);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE)
        {
            error_msg = "Failed to mark completed: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }
        Logger::debug("Marked job completed: " + src + " -> " + out);
        return WriteOperationResult(); });
    waitForWrites();
    return success;
}

bool DatabaseManager::markTranscodingJobFailed(const std::string &source_file_path)
{
    if (!waitForQueueInitialization())
    {
        std::string msg = "Access queue not initialized after retries";
        Logger::error(msg);
        return false;
    }
    std::string src = source_file_path;
    bool success = true;
    std::string error_msg;
    access_queue_->enqueueWrite([src, &success, &error_msg](DatabaseManager &dbMan)
                                {
        if (!dbMan.db_)
        {
            error_msg = "Database not initialized";
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }
        const std::string update_sql =
            "UPDATE cache_map SET status = 3, updated_at = CURRENT_TIMESTAMP WHERE source_file_path = ?";
        sqlite3_stmt *stmt = nullptr;
        int rc = sqlite3_prepare_v2(dbMan.db_, update_sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            error_msg = "Failed to prepare mark failed: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }
        sqlite3_bind_text(stmt, 1, src.c_str(), -1, SQLITE_STATIC);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE)
        {
            error_msg = "Failed to mark failed: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }
        Logger::debug("Marked job failed: " + src);
        return WriteOperationResult(); });
    waitForWrites();
    return success;
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
        
        // Only select files that actually need transcoding (RAW formats)
        // Dynamically build query based on enabled RAW formats from configuration
        // This makes the system truly configuration-driven
        auto transcoding_types = ServerConfigManager::getInstance().getTranscodingFileTypes();
        
        if (transcoding_types.empty())
        {
            Logger::info("No transcoding file types configured, returning empty list");
            return std::any(std::vector<std::string>());
        }
        
        // Build dynamic SQL query based on enabled RAW formats
        std::string query = R"(
            SELECT DISTINCT cm.source_file_path 
            FROM cache_map cm
            JOIN scanned_files sf ON cm.source_file_path = sf.file_path
            WHERE cm.transcoded_file_path IS NULL 
            AND (
        )";
        
        bool first = true;
        for (const auto& [extension, enabled] : transcoding_types)
        {
            if (enabled) // Only include enabled formats
            {
                if (!first)
                {
                    query += " OR ";
                }
                query += "LOWER(sf.file_name) LIKE '%." + extension + "'";
                first = false;
            }
        }
        
        query += ")";
        
        Logger::debug("Dynamic SQL query for transcoding: " + query);
        
        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare_v2(dbMan.db_, query.c_str(), -1, &stmt, nullptr);
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
        Logger::debug("Found " + std::to_string(files_needing_transcoding.size()) + " files that actually need transcoding");
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
    std::atomic<bool> operation_completed{false};
    std::atomic<bool> success{true};
    std::string error_msg;

    // Enqueue the write operation
    size_t operation_id = access_queue_->enqueueWrite([captured_file_path, captured_mode, &operation_completed, &success, &error_msg](DatabaseManager &dbMan)
                                                      {
        Logger::debug("Executing setProcessingFlag in write queue for: " + captured_file_path + " mode: " + DedupModes::getModeName(captured_mode));
        
        if (!dbMan.db_)
        {
            error_msg = "Database not initialized";
            Logger::error(error_msg);
            success.store(false);
            operation_completed.store(true);
            return WriteOperationResult::Failure(error_msg);
        }
        
        // Build the SQL query based on the mode - mark as completed (1) if currently in progress (-1) or not processed (0)
        std::string update_sql;
        switch (captured_mode)
        {
            case DedupMode::FAST:
                update_sql = "UPDATE scanned_files SET processed_fast = 1 WHERE file_path = ? AND (processed_fast = -1 OR processed_fast = 0)";
                break;
            case DedupMode::BALANCED:
                update_sql = "UPDATE scanned_files SET processed_balanced = 1 WHERE file_path = ? AND (processed_balanced = -1 OR processed_balanced = 0)";
                break;
            case DedupMode::QUALITY:
                update_sql = "UPDATE scanned_files SET processed_quality = 1 WHERE file_path = ? AND (processed_quality = -1 OR processed_quality = 0)";
                break;
            default:
                error_msg = "Unknown processing mode: " + DedupModes::getModeName(captured_mode);
                Logger::error(error_msg);
                success.store(false);
                operation_completed.store(true);
                return WriteOperationResult::Failure(error_msg);
        }
        
        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare_v2(dbMan.db_, update_sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            error_msg = "Failed to prepare update statement: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            success.store(false);
            operation_completed.store(true);
            return WriteOperationResult::Failure(error_msg);
        }

        sqlite3_bind_text(stmt, 1, captured_file_path.c_str(), -1, SQLITE_STATIC);

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE)
        {
            error_msg = "Failed to set processing flag: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            success.store(false);
            operation_completed.store(true);
            return WriteOperationResult::Failure(error_msg);
        }

        Logger::debug("Set processing flag for: " + captured_file_path + " mode: " + DedupModes::getModeName(captured_mode));
        operation_completed.store(true);
        return WriteOperationResult(); });

    // Wait for the operation to complete with timeout
    auto start_time = std::chrono::steady_clock::now();
    auto timeout = std::chrono::seconds(5); // 5 second timeout

    while (!operation_completed.load())
    {
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (elapsed > timeout)
        {
            Logger::error("setProcessingFlag timed out after 5 seconds for: " + file_path);
            return DBOpResult(false, "Operation timed out");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!success.load())
        return DBOpResult(false, error_msg);
    return DBOpResult(true);
}

// Reset processing flag for a specific mode after failed processing
DBOpResult DatabaseManager::resetProcessingFlag(const std::string &file_path, DedupMode mode)
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
    std::atomic<bool> operation_completed{false};
    std::atomic<bool> success{true};
    std::string error_msg;

    // Enqueue the write operation
    size_t operation_id = access_queue_->enqueueWrite([captured_file_path, captured_mode, &operation_completed, &success, &error_msg](DatabaseManager &dbMan)
                                                      {
        Logger::debug("Executing resetProcessingFlag in write queue for: " + captured_file_path + " mode: " + DedupModes::getModeName(captured_mode));
        
        if (!dbMan.db_)
        {
            error_msg = "Database not initialized";
            Logger::error(error_msg);
            success.store(false);
            operation_completed.store(true);
            return WriteOperationResult::Failure(error_msg);
        }
        
        // Build the SQL query based on the mode - reset to 0 if currently in progress (-1)
        std::string update_sql;
        switch (captured_mode)
        {
            case DedupMode::FAST:
                update_sql = "UPDATE scanned_files SET processed_fast = 0 WHERE file_path = ? AND processed_fast = -1";
                break;
            case DedupMode::BALANCED:
                update_sql = "UPDATE scanned_files SET processed_balanced = 0 WHERE file_path = ? AND processed_balanced = -1";
                break;
            case DedupMode::QUALITY:
                update_sql = "UPDATE scanned_files SET processed_quality = 0 WHERE file_path = ? AND processed_quality = -1";
                break;
            default:
                error_msg = "Unknown processing mode: " + DedupModes::getModeName(captured_mode);
                Logger::error(error_msg);
                success.store(false);
                operation_completed.store(true);
                return WriteOperationResult::Failure(error_msg);
        }
        
        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare_v2(dbMan.db_, update_sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            error_msg = "Failed to prepare update statement: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            success.store(false);
            operation_completed.store(true);
            return WriteOperationResult::Failure(error_msg);
        }

        sqlite3_bind_text(stmt, 1, captured_file_path.c_str(), -1, SQLITE_STATIC);

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE)
        {
            error_msg = "Failed to reset processing flag: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            success.store(false);
            operation_completed.store(true);
            return WriteOperationResult::Failure(error_msg);
        }

        Logger::debug("Reset processing flag for: " + captured_file_path + " mode: " + DedupModes::getModeName(captured_mode));
        operation_completed.store(true);
        return WriteOperationResult(); });

    // Wait for the operation to complete with timeout
    auto start_time = std::chrono::steady_clock::now();
    auto timeout = std::chrono::seconds(5); // 5 second timeout

    while (!operation_completed.load())
    {
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (elapsed > timeout)
        {
            Logger::error("resetProcessingFlag timed out after 5 seconds for: " + file_path);
            return DBOpResult(false, "Operation timed out");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!success.load())
        return DBOpResult(false, error_msg);
    return DBOpResult(true);
}

// Set processing flag to error state (2) for a specific mode
DBOpResult DatabaseManager::setProcessingFlagError(const std::string &file_path, DedupMode mode)
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
    std::atomic<bool> operation_completed{false};
    std::atomic<bool> success{true};
    std::string error_msg;

    // Enqueue the write operation
    size_t operation_id = access_queue_->enqueueWrite([captured_file_path, captured_mode, &operation_completed, &success, &error_msg](DatabaseManager &dbMan)
                                                      {
        Logger::debug("Executing setProcessingFlagError in write queue for: " + captured_file_path + " mode: " + DedupModes::getModeName(captured_mode));
        
        if (!dbMan.db_)
        {
            error_msg = "Database not initialized";
            Logger::error(error_msg);
            success.store(false);
            operation_completed.store(true);
            return WriteOperationResult::Failure(error_msg);
        }
        
        // Build the SQL query based on the mode - set to error state (2)
        std::string update_sql;
        switch (captured_mode)
        {
            case DedupMode::FAST:
                update_sql = "UPDATE scanned_files SET processed_fast = 2 WHERE file_path = ?";
                break;
            case DedupMode::BALANCED:
                update_sql = "UPDATE scanned_files SET processed_balanced = 2 WHERE file_path = ?";
                break;
            case DedupMode::QUALITY:
                update_sql = "UPDATE scanned_files SET processed_quality = 2 WHERE file_path = ?";
                break;
            default:
                error_msg = "Unknown processing mode: " + DedupModes::getModeName(captured_mode);
                Logger::error(error_msg);
                success.store(false);
                operation_completed.store(true);
                return WriteOperationResult::Failure(error_msg);
        }
        
        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare_v2(dbMan.db_, update_sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            error_msg = "Failed to prepare update statement: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            success.store(false);
            operation_completed.store(true);
            return WriteOperationResult::Failure(error_msg);
        }

        sqlite3_bind_text(stmt, 1, captured_file_path.c_str(), -1, SQLITE_STATIC);

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE)
        {
            error_msg = "Failed to set processing flag error: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            success.store(false);
            operation_completed.store(true);
            return WriteOperationResult::Failure(error_msg);
        }

        Logger::debug("Set processing flag to error state (2) for: " + captured_file_path + " mode: " + DedupModes::getModeName(captured_mode));
        operation_completed.store(true);
        return WriteOperationResult(); });

    // Wait for the operation to complete with timeout
    auto start_time = std::chrono::steady_clock::now();
    auto timeout = std::chrono::seconds(5); // 5 second timeout

    while (!operation_completed.load())
    {
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (elapsed > timeout)
        {
            Logger::error("setProcessingFlagError timed out after 5 seconds for: " + file_path);
            return DBOpResult(false, "Operation timed out");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!success.load())
        return DBOpResult(false, error_msg);
    return DBOpResult(true);
}

// Set processing flag to transcoding error state (3) for a specific mode
DBOpResult DatabaseManager::setProcessingFlagTranscodingError(const std::string &file_path, DedupMode mode)
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
    std::atomic<bool> operation_completed{false};
    std::atomic<bool> success{true};
    std::string error_msg;

    // Enqueue the write operation
    size_t operation_id = access_queue_->enqueueWrite([captured_file_path, captured_mode, &operation_completed, &success, &error_msg](DatabaseManager &dbMan)
                                                      {
        Logger::debug("Executing setProcessingFlagTranscodingError in write queue for: " + captured_file_path + " mode: " + DedupModes::getModeName(captured_mode));
        
        if (!dbMan.db_)
        {
            error_msg = "Database not initialized";
            Logger::error(error_msg);
            success.store(false);
            operation_completed.store(true);
            return WriteOperationResult::Failure(error_msg);
        }
        
        // Build the SQL query based on the mode - set to transcoding error state (3)
        std::string update_sql;
        switch (captured_mode)
        {
            case DedupMode::FAST:
                update_sql = "UPDATE scanned_files SET processed_fast = 3 WHERE file_path = ?";
                break;
            case DedupMode::BALANCED:
                update_sql = "UPDATE scanned_files SET processed_balanced = 3 WHERE file_path = ?";
                break;
            case DedupMode::QUALITY:
                update_sql = "UPDATE scanned_files SET processed_quality = 3 WHERE file_path = ?";
                break;
            default:
                error_msg = "Unknown processing mode: " + DedupModes::getModeName(captured_mode);
                Logger::error(error_msg);
                success.store(false);
                operation_completed.store(true);
                return WriteOperationResult::Failure(error_msg);
        }
        
        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare_v2(dbMan.db_, update_sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            error_msg = "Failed to prepare update statement: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            success.store(false);
            operation_completed.store(true);
            return WriteOperationResult::Failure(error_msg);
        }

        sqlite3_bind_text(stmt, 1, captured_file_path.c_str(), -1, SQLITE_STATIC);

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE)
        {
            error_msg = "Failed to set processing flag transcoding error: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            success.store(false);
            operation_completed.store(true);
            return WriteOperationResult::Failure(error_msg);
        }

        Logger::debug("Set processing flag to transcoding error state (3) for: " + captured_file_path + " mode: " + DedupModes::getModeName(captured_mode));
        operation_completed.store(true);
        return WriteOperationResult(); });

    // Wait for the operation to complete with timeout
    auto start_time = std::chrono::steady_clock::now();
    auto timeout = std::chrono::seconds(5); // 5 second timeout

    while (!operation_completed.load())
    {
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (elapsed > timeout)
        {
            Logger::error("setProcessingFlagTranscodingError timed out after 5 seconds for: " + file_path);
            return DBOpResult(false, "Operation timed out");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!success.load())
        return DBOpResult(false, error_msg);
    return DBOpResult(true);
}

// Set processing flag to final error state (4) for a specific mode
DBOpResult DatabaseManager::setProcessingFlagFinalError(const std::string &file_path, DedupMode mode)
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
    std::atomic<bool> operation_completed{false};
    std::atomic<bool> success{true};
    std::string error_msg;

    // Enqueue the write operation
    size_t operation_id = access_queue_->enqueueWrite([captured_file_path, captured_mode, &operation_completed, &success, &error_msg](DatabaseManager &dbMan)
                                                      {
        Logger::debug("Executing setProcessingFlagFinalError in write queue for: " + captured_file_path + " mode: " + DedupModes::getModeName(captured_mode));
        
        if (!dbMan.db_)
        {
            error_msg = "Database not initialized";
            Logger::error(error_msg);
            success.store(false);
            operation_completed.store(true);
            return WriteOperationResult::Failure(error_msg);
        }
        
        // Build the SQL query based on the mode - set to final error state (4)
        std::string update_sql;
        switch (captured_mode)
        {
            case DedupMode::FAST:
                update_sql = "UPDATE scanned_files SET processed_fast = 4 WHERE file_path = ?";
                break;
            case DedupMode::BALANCED:
                update_sql = "UPDATE scanned_files SET processed_balanced = 4 WHERE file_path = ?";
                break;
            case DedupMode::QUALITY:
                update_sql = "UPDATE scanned_files SET processed_quality = 4 WHERE file_path = ?";
                break;
            default:
                error_msg = "Unknown processing mode: " + DedupModes::getModeName(captured_mode);
                Logger::error(error_msg);
                success.store(false);
                operation_completed.store(true);
                return WriteOperationResult::Failure(error_msg);
        }
        
        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare_v2(dbMan.db_, update_sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            error_msg = "Failed to prepare update statement: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            success.store(false);
            operation_completed.store(true);
            return WriteOperationResult::Failure(error_msg);
        }

        sqlite3_bind_text(stmt, 1, captured_file_path.c_str(), -1, SQLITE_STATIC);

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE)
        {
            error_msg = "Failed to set processing flag final error: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            success.store(false);
            operation_completed.store(true);
            return WriteOperationResult::Failure(error_msg);
        }

        Logger::debug("Set processing flag to final error state (4) for: " + captured_file_path + " mode: " + DedupModes::getModeName(captured_mode));
        operation_completed.store(true);
        return WriteOperationResult(); });

    // Wait for the operation to complete with timeout
    auto start_time = std::chrono::steady_clock::now();
    auto timeout = std::chrono::seconds(5); // 5 second timeout

    while (!operation_completed.load())
    {
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (elapsed > timeout)
        {
            Logger::error("setProcessingFlagFinalError timed out after 5 seconds for: " + file_path);
            return DBOpResult(false, "Operation timed out");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!success.load())
        return DBOpResult(false, error_msg);
    return DBOpResult(true);
}

std::vector<std::string> DatabaseManager::getFilesWithProcessingFlag(int flag_value, DedupMode mode)
{
    if (!waitForQueueInitialization())
    {
        Logger::error("Access queue not initialized after retries");
        return {};
    }

    // Capture parameters for async execution
    int captured_flag_value = flag_value;
    DedupMode captured_mode = mode;
    std::vector<std::string> results;

    // Enqueue the read operation
    auto future = access_queue_->enqueueRead([captured_flag_value, captured_mode, &results](DatabaseManager &dbMan)
                                             {
        Logger::debug("Executing getFilesWithProcessingFlag in read queue for flag value: " + std::to_string(captured_flag_value) + " mode: " + DedupModes::getModeName(captured_mode));
        
        if (!dbMan.db_)
        {
            Logger::error("Database not initialized");
            return std::any(std::vector<std::string>());
        }
        
        // Build the SQL query based on the mode
        std::string select_sql;
        switch (captured_mode)
        {
            case DedupMode::FAST:
                select_sql = "SELECT file_path FROM scanned_files WHERE processed_fast = ?";
                break;
            case DedupMode::BALANCED:
                select_sql = "SELECT file_path FROM scanned_files WHERE processed_balanced = ?";
                break;
            case DedupMode::QUALITY:
                select_sql = "SELECT file_path FROM scanned_files WHERE processed_quality = ?";
                break;
            default:
                Logger::error("Unknown processing mode: " + DedupModes::getModeName(captured_mode));
                return std::any(std::vector<std::string>());
        }
        
        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare_v2(dbMan.db_, select_sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            Logger::error("Failed to prepare select statement: " + std::string(sqlite3_errmsg(dbMan.db_)));
            return std::any(std::vector<std::string>());
        }

        sqlite3_bind_int(stmt, 1, captured_flag_value);
        
        std::vector<std::string> file_paths;
        while (sqlite3_step(stmt) == SQLITE_ROW)
        {
            const char *file_path = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
            if (file_path)
            {
                file_paths.push_back(std::string(file_path));
            }
        }
        
        sqlite3_finalize(stmt);
        
        Logger::debug("Found " + std::to_string(file_paths.size()) + " files with processing flag " + std::to_string(captured_flag_value) + " for mode " + DedupModes::getModeName(captured_mode));
        return std::any(file_paths); });

    try
    {
        auto result = future.get();
        if (result.has_value())
        {
            results = std::any_cast<std::vector<std::string>>(result);
        }
    }
    catch (const std::exception &e)
    {
        Logger::error("Error getting files with processing flag: " + std::string(e.what()));
    }

    return results;
}

int DatabaseManager::getProcessingFlag(const std::string &file_path, DedupMode mode)
{
    if (!waitForQueueInitialization())
    {
        Logger::error("Access queue not initialized after retries");
        return -1;
    }

    // Capture parameters for async execution
    std::string captured_file_path = file_path;
    DedupMode captured_mode = mode;
    int flag_value = -1;

    // Enqueue the read operation
    auto future = access_queue_->enqueueRead([captured_file_path, captured_mode, &flag_value](DatabaseManager &dbMan)
                                             {
        Logger::debug("Executing getProcessingFlag in read queue for: " + captured_file_path + " mode: " + DedupModes::getModeName(captured_mode));
        
        if (!dbMan.db_)
        {
            Logger::error("Database not initialized");
            return std::any(-1);
        }
        
        // Build the SQL query based on the mode
        std::string select_sql;
        switch (captured_mode)
        {
            case DedupMode::FAST:
                select_sql = "SELECT processed_fast FROM scanned_files WHERE file_path = ?";
                break;
            case DedupMode::BALANCED:
                select_sql = "SELECT processed_balanced FROM scanned_files WHERE file_path = ?";
                break;
            case DedupMode::QUALITY:
                select_sql = "SELECT processed_quality FROM scanned_files WHERE file_path = ?";
                break;
            default:
                Logger::error("Unknown processing mode: " + DedupModes::getModeName(captured_mode));
                return std::any(-1);
        }
        
        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare_v2(dbMan.db_, select_sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            Logger::error("Failed to prepare select statement: " + std::string(sqlite3_errmsg(dbMan.db_)));
            return std::any(-1);
        }

        sqlite3_bind_text(stmt, 1, captured_file_path.c_str(), -1, SQLITE_STATIC);
        
        int result_flag = -1;
        if (sqlite3_step(stmt) == SQLITE_ROW)
        {
            result_flag = sqlite3_column_int(stmt, 0);
        }
        
        sqlite3_finalize(stmt);
        
        Logger::debug("Processing flag for " + captured_file_path + " mode " + DedupModes::getModeName(captured_mode) + ": " + std::to_string(result_flag));
        return std::any(result_flag); });

    try
    {
        auto result = future.get();
        if (result.has_value())
        {
            flag_value = std::any_cast<int>(result);
        }
    }
    catch (const std::exception &e)
    {
        Logger::error("Error getting processing flag: " + std::string(e.what()));
    }

    return flag_value;
}

bool DatabaseManager::tryAcquireProcessingLock(const std::string &file_path, DedupMode mode)
{
    if (!waitForQueueInitialization())
    {
        Logger::error("Access queue not initialized after retries");
        return false;
    }

    // Capture parameters for async execution by value to avoid reference issues
    std::string captured_file_path = file_path;
    DedupMode captured_mode = mode;
    std::atomic<bool> lock_acquired{false};
    std::atomic<bool> operation_completed{false};
    std::string error_msg;

    // Enqueue the write operation
    size_t operation_id = access_queue_->enqueueWrite([captured_file_path, captured_mode, &lock_acquired, &operation_completed, &error_msg](DatabaseManager &dbMan)
                                                      {
        Logger::debug("Executing tryAcquireProcessingLockAtomic in write queue for: " + captured_file_path + " mode: " + DedupModes::getModeName(captured_mode));
        
        if (!dbMan.db_)
        {
            error_msg = "Database not initialized";
            Logger::error(error_msg);
            lock_acquired.store(false);
            operation_completed.store(true);
            return WriteOperationResult::Failure(error_msg);
        }
        
        // Build the SQL query based on the mode - only update if not already processed
        std::string update_sql;
        switch (captured_mode)
        {
            case DedupMode::FAST:
                update_sql = "UPDATE scanned_files SET processed_fast = -1 WHERE file_path = ? AND processed_fast = 0";
                break;
            case DedupMode::BALANCED:
                update_sql = "UPDATE scanned_files SET processed_balanced = -1 WHERE file_path = ? AND processed_balanced = 0";
                break;
            case DedupMode::QUALITY:
                update_sql = "UPDATE scanned_files SET processed_quality = -1 WHERE file_path = ? AND processed_quality = 0";
                break;
            default:
                error_msg = "Invalid dedup mode";
                Logger::error(error_msg);
                lock_acquired.store(false);
                operation_completed.store(true);
                return WriteOperationResult::Failure(error_msg);
        }
        
        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare_v2(dbMan.db_, update_sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            error_msg = "Failed to prepare atomic update statement: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            lock_acquired.store(false);
            operation_completed.store(true);
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
            operation_completed.store(true);
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

        operation_completed.store(true);
        return WriteOperationResult(); });

    // Wait for the operation to complete with timeout
    auto start_time = std::chrono::steady_clock::now();
    auto timeout = std::chrono::seconds(5); // 5 second timeout

    while (!operation_completed.load())
    {
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (elapsed > timeout)
        {
            Logger::error("tryAcquireProcessingLock timed out after 5 seconds for: " + file_path);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return lock_acquired.load();
}

std::vector<std::pair<std::string, std::string>> DatabaseManager::getAndMarkFilesForProcessing(DedupMode mode, int batch_size)
{
    Logger::debug("getAndMarkFilesForProcessing called for mode: " + DedupModes::getModeName(mode) + " with batch size: " + std::to_string(batch_size));
    std::vector<std::pair<std::string, std::string>> results;
    if (!waitForQueueInitialization())
    {
        Logger::error("Access queue not initialized after retries");
        return results;
    }

    // Use a mutex to ensure only one thread can get files at a time
    std::lock_guard<std::mutex> lock(file_processing_mutex);

    // Capture the parameters for async execution
    DedupMode captured_mode = mode;
    int captured_batch_size = batch_size;
    std::atomic<bool> operation_completed{false};
    std::string error_msg;

    // Use enqueueWrite since we're performing write operations (UPDATE statements)
    size_t operation_id = access_queue_->enqueueWrite([captured_mode, captured_batch_size, &results, &operation_completed, &error_msg, this](DatabaseManager &dbMan)
                                                      {
        Logger::debug("Executing getAndMarkFilesForProcessing in write queue for mode: " + DedupModes::getModeName(captured_mode));
        
        if (!dbMan.db_)
        {
            error_msg = "Database not initialized";
            Logger::error(error_msg);
            operation_completed.store(true);
            return WriteOperationResult::Failure(error_msg);
        }
        
        // Start transaction with immediate mode for better concurrency
        sqlite3_exec(dbMan.db_, "BEGIN IMMEDIATE TRANSACTION", nullptr, nullptr, nullptr);
        
        // Build the SQL query to get files that need processing
        // Exclude files that are already marked as in progress (-1) to prevent race conditions
        std::string select_sql;
        std::string file_type_clauses = generateFileTypeLikeClauses();
        
        switch (captured_mode)
        {
            case DedupMode::FAST:
                select_sql = "SELECT file_path, file_name FROM scanned_files WHERE processed_fast = 0 AND processed_fast != -1 AND (" + file_type_clauses + ") ORDER BY created_at DESC LIMIT ?";
                break;
            case DedupMode::BALANCED:
                select_sql = "SELECT file_path, file_name FROM scanned_files WHERE processed_balanced = 0 AND processed_balanced != -1 AND (" + file_type_clauses + ") ORDER BY created_at DESC LIMIT ?";
                break;
            case DedupMode::QUALITY:
                select_sql = "SELECT file_path, file_name FROM scanned_files WHERE processed_quality = 0 AND processed_quality != -1 AND (" + file_type_clauses + ") ORDER BY created_at DESC LIMIT ?";
                break;
        }
        
        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare_v2(dbMan.db_, select_sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            Logger::error("Failed to prepare select statement: " + std::string(sqlite3_errmsg(dbMan.db_)));
            sqlite3_exec(dbMan.db_, "ROLLBACK", nullptr, nullptr, nullptr);
            operation_completed.store(true);
            return WriteOperationResult::Failure("Failed to prepare select statement");
        }

        sqlite3_bind_int(stmt, 1, captured_batch_size);

        std::vector<std::string> file_paths_to_mark;
        while (sqlite3_step(stmt) == SQLITE_ROW)
        {
            std::string file_path = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
            std::string file_name = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
            results.emplace_back(file_path, file_name);
            file_paths_to_mark.push_back(file_path);
        }

        sqlite3_finalize(stmt);

        // If we found files, mark them as in progress
        if (!file_paths_to_mark.empty())
        {
            // Build the update SQL
            std::string update_sql;
            switch (captured_mode)
            {
                case DedupMode::FAST:
                    update_sql = "UPDATE scanned_files SET processed_fast = -1 WHERE file_path = ?";
                    break;
                case DedupMode::BALANCED:
                    update_sql = "UPDATE scanned_files SET processed_balanced = -1 WHERE file_path = ?";
                    break;
                case DedupMode::QUALITY:
                    update_sql = "UPDATE scanned_files SET processed_quality = -1 WHERE file_path = ?";
                    break;
                default:
                    break;
            }

            sqlite3_stmt *update_stmt;
            rc = sqlite3_prepare_v2(dbMan.db_, update_sql.c_str(), -1, &update_stmt, nullptr);
            if (rc != SQLITE_OK)
            {
                Logger::error("Failed to prepare update statement: " + std::string(sqlite3_errmsg(dbMan.db_)));
                sqlite3_exec(dbMan.db_, "ROLLBACK", nullptr, nullptr, nullptr);
                operation_completed.store(true);
                return WriteOperationResult::Failure("Failed to prepare update statement");
            }

            // Mark each file as in progress
            for (const auto& file_path : file_paths_to_mark)
            {
                sqlite3_bind_text(update_stmt, 1, file_path.c_str(), -1, SQLITE_STATIC);
                rc = sqlite3_step(update_stmt);
                if (rc != SQLITE_DONE)
                {
                    Logger::error("Failed to mark file as in progress: " + file_path + " - " + std::string(sqlite3_errmsg(dbMan.db_)));
                }
                sqlite3_reset(update_stmt);
            }

            sqlite3_finalize(update_stmt);
        }

        // Commit transaction
        sqlite3_exec(dbMan.db_, "COMMIT", nullptr, nullptr, nullptr);
        
        Logger::debug("Atomically marked " + std::to_string(results.size()) + " files as in progress for mode: " + DedupModes::getModeName(captured_mode));
        operation_completed.store(true);
        return WriteOperationResult(); });

    // Wait for the operation to complete with timeout
    auto start_time = std::chrono::steady_clock::now();
    auto timeout = std::chrono::seconds(10); // 10 second timeout

    while (!operation_completed.load())
    {
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (elapsed > timeout)
        {
            Logger::error("getAndMarkFilesForProcessing timed out after 10 seconds");
            return results;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return results;
}

std::vector<std::pair<std::string, std::string>> DatabaseManager::getAndMarkFilesForProcessingAnyMode(int batch_size)
{
    Logger::debug("getAndMarkFilesForProcessingAnyMode called with batch size: " + std::to_string(batch_size));
    std::vector<std::pair<std::string, std::string>> results;
    if (!waitForQueueInitialization())
    {
        Logger::error("Access queue not initialized after retries");
        return results;
    }

    // Use a mutex to ensure only one thread can get files at a time
    std::lock_guard<std::mutex> lock(file_processing_mutex);

    // Capture the parameters for async execution
    int captured_batch_size = batch_size;
    std::atomic<bool> operation_completed{false};
    std::string error_msg;
    auto shared_results = std::make_shared<std::vector<std::pair<std::string, std::string>>>();

    // Use enqueueWrite since we're performing write operations (UPDATE statements)
    auto future = access_queue_->enqueueWrite([captured_batch_size, &operation_completed, &error_msg, shared_results, this](DatabaseManager &dbMan) -> WriteOperationResult
                                              {
        Logger::debug("Executing getAndMarkFilesForProcessingAnyMode in write queue");
        
        if (!dbMan.db_)
        {
            error_msg = "Database not initialized";
            Logger::error(error_msg);
            operation_completed.store(true);
            return WriteOperationResult::Failure(error_msg);
        }
        
        // Start transaction with immediate mode for better concurrency
        sqlite3_exec(dbMan.db_, "BEGIN IMMEDIATE TRANSACTION", nullptr, nullptr, nullptr);
        
        // Build the SQL query to get files that need processing for ANY mode
        // Use a more precise approach to avoid race conditions
        std::string file_type_clauses = generateFileTypeLikeClauses();
        
        std::string select_sql = "SELECT file_path, file_name FROM scanned_files WHERE "
                                "(" + file_type_clauses + ") AND "
                                "((processed_fast = 0 AND processed_fast != -1) OR "
                                "(processed_balanced = 0 AND processed_balanced != -1) OR "
                                "(processed_quality = 0 AND processed_quality != -1)) "
                                "ORDER BY created_at DESC LIMIT ?";
        
        Logger::debug("File type clauses: " + file_type_clauses);
        Logger::debug("SQL query: " + select_sql);
        
        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare_v2(dbMan.db_, select_sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            Logger::error("Failed to prepare select statement: " + std::string(sqlite3_errmsg(dbMan.db_)));
            sqlite3_exec(dbMan.db_, "ROLLBACK", nullptr, nullptr, nullptr);
            operation_completed.store(true);
            return WriteOperationResult::Failure("Failed to prepare select statement");
        }

        sqlite3_bind_int(stmt, 1, captured_batch_size);

        std::vector<std::string> file_paths_to_mark;
        std::vector<std::pair<std::string, std::string>> local_results;
        Logger::debug("Executing query with batch size: " + std::to_string(captured_batch_size));
        
        while (sqlite3_step(stmt) == SQLITE_ROW)
        {
            std::string file_path = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
            std::string file_name = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
            shared_results->emplace_back(file_path, file_name);
            file_paths_to_mark.push_back(file_path);
            Logger::debug("Found file needing processing: " + file_path);
        }
        
        Logger::debug("Total files found: " + std::to_string(shared_results->size()));

        sqlite3_finalize(stmt);

        // If we found files, mark them as in progress for ALL modes that need processing
        if (!file_paths_to_mark.empty())
        {
            // Mark files as in progress for each mode that needs processing
            std::vector<std::string> update_sqls = {
                "UPDATE scanned_files SET processed_fast = -1 WHERE file_path = ? AND processed_fast = 0",
                "UPDATE scanned_files SET processed_balanced = -1 WHERE file_path = ? AND processed_balanced = 0",
                "UPDATE scanned_files SET processed_quality = -1 WHERE file_path = ? AND processed_quality = 0"
            };

            for (const auto& update_sql : update_sqls)
            {
                sqlite3_stmt *update_stmt;
                rc = sqlite3_prepare_v2(dbMan.db_, update_sql.c_str(), -1, &update_stmt, nullptr);
                if (rc != SQLITE_OK)
                {
                    Logger::error("Failed to prepare update statement: " + std::string(sqlite3_errmsg(dbMan.db_)));
                    sqlite3_exec(dbMan.db_, "ROLLBACK", nullptr, nullptr, nullptr);
                    operation_completed.store(true);
                    return WriteOperationResult::Failure("Failed to prepare update statement");
                }

                // Mark each file as in progress for this mode
                for (const auto& file_path : file_paths_to_mark)
                {
                    sqlite3_bind_text(update_stmt, 1, file_path.c_str(), -1, SQLITE_STATIC);
                    rc = sqlite3_step(update_stmt);
                    if (rc != SQLITE_DONE)
                    {
                        Logger::error("Failed to mark file as in progress: " + file_path + " - " + std::string(sqlite3_errmsg(dbMan.db_)));
                    }
                    sqlite3_reset(update_stmt);
                }

                sqlite3_finalize(update_stmt);
            }
        }

        // Commit transaction
        sqlite3_exec(dbMan.db_, "COMMIT", nullptr, nullptr, nullptr);
        
        Logger::debug("Atomically marked " + std::to_string(shared_results->size()) + " files as in progress for any mode");
        operation_completed.store(true);
        return WriteOperationResult(true); });

    // Wait for the operation to complete with timeout
    auto start_time = std::chrono::steady_clock::now();
    auto timeout = std::chrono::seconds(10); // 10 second timeout

    while (!operation_completed.load())
    {
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (elapsed > timeout)
        {
            Logger::error("getAndMarkFilesForProcessingAnyMode timed out after 10 seconds");
            return results;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Copy results from shared pointer
    results = *shared_results;

    return results;
}

// Helper function to generate SQL LIKE clauses for enabled file types
std::string DatabaseManager::generateFileTypeLikeClauses()
{
    auto enabled_types = ServerConfigManager::getInstance().getEnabledFileTypes();
    if (enabled_types.empty())
    {
        return "1=0"; // No enabled types, return false condition
    }

    std::string clauses;
    for (size_t i = 0; i < enabled_types.size(); ++i)
    {
        if (i > 0)
        {
            clauses += " OR ";
        }
        clauses += "LOWER(file_name) LIKE '%." + enabled_types[i] + "'";
    }
    return clauses;
}

std::vector<std::pair<std::string, std::string>> DatabaseManager::getFilesNeedingProcessingAnyMode(int batch_size)
{
    Logger::debug("getFilesNeedingProcessingAnyMode called with batch size: " + std::to_string(batch_size));
    std::vector<std::pair<std::string, std::string>> results;
    if (!waitForQueueInitialization())
    {
        Logger::error("Access queue not initialized after retries");
        return results;
    }

    // Capture the parameters for async execution
    int captured_batch_size = batch_size;
    std::atomic<bool> operation_completed{false};
    std::string error_msg;

    // Use enqueueRead since we're only reading files that need processing
    auto future = access_queue_->enqueueRead([captured_batch_size, &results, &operation_completed, &error_msg, this](DatabaseManager &dbMan) -> std::any
                                             {
        Logger::debug("Executing getFilesNeedingProcessingAnyMode in read queue");
        
        if (!dbMan.db_)
        {
            error_msg = "Database not initialized";
            Logger::error(error_msg);
            operation_completed.store(true);
            return std::any();
        }
        
        // Build the SQL query to get files that need processing for ANY mode
        std::string file_type_clauses = generateFileTypeLikeClauses();
        std::string select_sql = "SELECT file_path, file_name FROM scanned_files WHERE (" + file_type_clauses + ") AND (processed_fast = 0 OR processed_balanced = 0 OR processed_quality = 0) ORDER BY created_at DESC LIMIT ?";
        
        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare_v2(dbMan.db_, select_sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            Logger::error("Failed to prepare select statement: " + std::string(sqlite3_errmsg(dbMan.db_)));
            operation_completed.store(true);
            return std::any();
        }

        sqlite3_bind_int(stmt, 1, captured_batch_size);

        while (sqlite3_step(stmt) == SQLITE_ROW)
        {
            std::string file_path = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
            std::string file_name = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
            results.emplace_back(file_path, file_name);
        }

        sqlite3_finalize(stmt);
        
        Logger::debug("Found " + std::to_string(results.size()) + " files needing processing for any mode");
        operation_completed.store(true);
        return std::any(); });

    // Wait for the operation to complete with timeout
    auto start_time = std::chrono::steady_clock::now();
    auto timeout = std::chrono::seconds(10); // 10 second timeout

    while (!operation_completed.load())
    {
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (elapsed > timeout)
        {
            Logger::error("getFilesNeedingProcessingAnyMode timed out after 10 seconds");
            return results;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Wait for the future to complete
    try
    {
        future.wait();
    }
    catch (const std::exception &e)
    {
        Logger::error("Exception waiting for read operation: " + std::string(e.what()));
    }

    return results;
}

DatabaseManager::ServerStatus DatabaseManager::getServerStatus()
{
    ServerStatus status = {0, 0, 0, 0, 0};

    if (!waitForQueueInitialization())
    {
        Logger::error("Access queue not initialized for getServerStatus");
        return status;
    }

    auto future = access_queue_->enqueueRead([&status](DatabaseManager &dbMan)
                                             {
        if (!dbMan.db_)
        {
            Logger::error("Database not initialized in getServerStatus");
            return std::any(status);
        }

        try
        {
            // 1. Total files scanned
            const std::string scanned_sql = "SELECT COUNT(*) FROM scanned_files";
            sqlite3_stmt *scanned_stmt = nullptr;
            if (sqlite3_prepare_v2(dbMan.db_, scanned_sql.c_str(), -1, &scanned_stmt, nullptr) == SQLITE_OK)
            {
                if (sqlite3_step(scanned_stmt) == SQLITE_ROW)
                {
                    status.files_scanned = static_cast<size_t>(sqlite3_column_int(scanned_stmt, 0));
                }
                sqlite3_finalize(scanned_stmt);
            }

            // 2. Files processed (in any mode)
            const std::string processed_sql = "SELECT COUNT(DISTINCT file_path) FROM media_processing_results WHERE success = 1";
            sqlite3_stmt *processed_stmt = nullptr;
            if (sqlite3_prepare_v2(dbMan.db_, processed_sql.c_str(), -1, &processed_stmt, nullptr) == SQLITE_OK)
            {
                if (sqlite3_step(processed_stmt) == SQLITE_ROW)
                {
                    status.files_processed = static_cast<size_t>(sqlite3_column_int(processed_stmt, 0));
                }
                sqlite3_finalize(processed_stmt);
            }

            // 3. Files queued (scanned but not processed)
            status.files_queued = status.files_scanned - status.files_processed;

            // 4. Duplicates found (files with non-empty links in any mode)
            const std::string duplicates_sql = "SELECT COUNT(*) FROM scanned_files WHERE (links_fast IS NOT NULL AND links_fast != '') OR (links_balanced IS NOT NULL AND links_balanced != '') OR (links_quality IS NOT NULL AND links_quality != '')";
            sqlite3_stmt *duplicates_stmt = nullptr;
            if (sqlite3_prepare_v2(dbMan.db_, duplicates_sql.c_str(), -1, &duplicates_stmt, nullptr) == SQLITE_OK)
            {
                if (sqlite3_step(duplicates_stmt) == SQLITE_ROW)
                {
                    status.duplicates_found = static_cast<size_t>(sqlite3_column_int(duplicates_stmt, 0));
                }
                sqlite3_finalize(duplicates_stmt);
            }

            // 5. Files in error (files with success = 0 in media_processing_results)
            const std::string error_sql = "SELECT COUNT(DISTINCT file_path) FROM media_processing_results WHERE success = 0";
            sqlite3_stmt *error_stmt = nullptr;
            if (sqlite3_prepare_v2(dbMan.db_, error_sql.c_str(), -1, &error_stmt, nullptr) == SQLITE_OK)
            {
                if (sqlite3_step(error_stmt) == SQLITE_ROW)
                {
                    status.files_in_error = static_cast<size_t>(sqlite3_column_int(error_stmt, 0));
                }
                sqlite3_finalize(error_stmt);
            }

            Logger::debug("Server status retrieved - Scanned: " + std::to_string(status.files_scanned) + 
                         ", Queued: " + std::to_string(status.files_queued) + 
                         ", Processed: " + std::to_string(status.files_processed) + 
                         ", Duplicates: " + std::to_string(status.duplicates_found) +
                         ", Errors: " + std::to_string(status.files_in_error));
        }
        catch (const std::exception &e)
        {
            Logger::error("Exception in getServerStatus: " + std::string(e.what()));
        }

        return std::any(status); });

    try
    {
        status = std::any_cast<ServerStatus>(future.get());
    }
    catch (const std::exception &e)
    {
        Logger::error("Error getting server status from access queue: " + std::string(e.what()));
    }

    return status;
}

DBOpResult DatabaseManager::setFileLinksForMode(const std::string &file_path, const std::vector<int> &linked_ids, DedupMode mode)
{
    if (!waitForQueueInitialization())
    {
        std::string msg = "Access queue not initialized after retries";
        Logger::error(msg);
        return DBOpResult(false, msg);
    }

    // Convert vector to comma-separated string
    std::stringstream ss;
    for (size_t i = 0; i < linked_ids.size(); ++i)
    {
        if (i > 0)
            ss << ",";
        ss << linked_ids[i];
    }
    std::string links_text = ss.str();

    // Determine which field to update based on mode
    std::string field_name;
    switch (mode)
    {
    case DedupMode::FAST:
        field_name = "links_fast";
        break;
    case DedupMode::BALANCED:
        field_name = "links_balanced";
        break;
    case DedupMode::QUALITY:
        field_name = "links_quality";
        break;
    default:
        return DBOpResult(false, "Invalid deduplication mode");
    }

    // Capture parameters for async execution
    std::string captured_file_path = file_path;
    std::string captured_links_text = links_text;
    std::string captured_field_name = field_name;
    std::string error_msg;
    bool success = true;

    // Enqueue the write operation
    access_queue_->enqueueWrite([captured_file_path, captured_links_text, captured_field_name, &error_msg, &success](DatabaseManager &dbMan)
                                {
        Logger::debug("Executing setFileLinksForMode in write queue for: " + captured_file_path + " mode: " + captured_field_name);
        
        const std::string update_sql = "UPDATE scanned_files SET " + captured_field_name + " = ? WHERE file_path = ?";
        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare_v2(dbMan.db_, update_sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            error_msg = "Failed to prepare update statement: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }

        sqlite3_bind_text(stmt, 1, captured_links_text.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, captured_file_path.c_str(), -1, SQLITE_STATIC);

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE)
        {
            error_msg = "Failed to update file links for mode " + captured_field_name + ": " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }

        Logger::debug("Updated file links for: " + captured_file_path + " mode: " + captured_field_name);
        return WriteOperationResult(); });

    waitForWrites();
    if (!success)
        return DBOpResult(false, error_msg);
    return DBOpResult(true, "");
}

std::vector<int> DatabaseManager::getFileLinksForMode(const std::string &file_path, DedupMode mode)
{
    std::vector<int> results;
    if (!waitForQueueInitialization())
    {
        Logger::error("Access queue not initialized after retries");
        return results;
    }

    // Determine which field to query based on mode
    std::string field_name;
    switch (mode)
    {
    case DedupMode::FAST:
        field_name = "links_fast";
        break;
    case DedupMode::BALANCED:
        field_name = "links_balanced";
        break;
    case DedupMode::QUALITY:
        field_name = "links_quality";
        break;
    default:
        Logger::error("Invalid deduplication mode");
        return results;
    }

    // Capture parameters for async execution
    std::string captured_file_path = file_path;
    std::string captured_field_name = field_name;

    // Enqueue the read operation
    auto future = access_queue_->enqueueRead([captured_file_path, captured_field_name](DatabaseManager &dbMan)
                                             {
        Logger::debug("Executing getFileLinksForMode in access queue for: " + captured_file_path + " mode: " + captured_field_name);
        
        if (!dbMan.db_)
        {
            Logger::error("Database not initialized");
            return std::any(std::vector<int>());
        }
        
        std::vector<int> results;
        const std::string select_sql = "SELECT " + captured_field_name + " FROM scanned_files WHERE file_path = ?";
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
                std::string links_text = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
                if (!links_text.empty())
                {
                    std::stringstream ss(links_text);
                    std::string id_str;
                    while (std::getline(ss, id_str, ','))
                    {
                        try
                        {
                            int id = std::stoi(id_str);
                            results.push_back(id);
                        }
                        catch (const std::exception &e)
                        {
                            Logger::error("Failed to parse link ID: " + id_str + " - " + std::string(e.what()));
                        }
                    }
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
        Logger::error("Failed to get file links for mode: " + std::string(e.what()));
    }

    return results;
}

std::vector<int> DatabaseManager::getFileLinksForCurrentMode(const std::string &file_path)
{
    // Get the current deduplication mode from the server configuration
    try
    {
        auto &config = ServerConfigManager::getInstance();
        DedupMode current_mode = config.getDedupMode();
        return getFileLinksForMode(file_path, current_mode);
    }
    catch (const std::exception &e)
    {
        Logger::error("Failed to get current deduplication mode: " + std::string(e.what()));
        return std::vector<int>();
    }
}

std::pair<bool, std::string> DatabaseManager::getTableHash(const std::string &table_name)
{
    Logger::debug("getTableHash called for table: " + table_name);

    if (!waitForQueueInitialization())
    {
        Logger::error("Access queue not initialized after retries");
        return {false, "Access queue not initialized"};
    }

    // Capture table name for async execution
    std::string captured_table_name = table_name;

    // Enqueue the read operation
    auto future = access_queue_->enqueueRead([captured_table_name](DatabaseManager &dbMan)
                                             {
        Logger::debug("Executing getTableHash in access queue for table: " + captured_table_name);
        
        if (!dbMan.db_)
        {
            Logger::error("Database not initialized");
            return std::any(std::pair<bool, std::string>(false, "Database not initialized"));
        }
        
        // First, check if the table exists
        const std::string check_sql = "SELECT name FROM sqlite_master WHERE type='table' AND name=?";
        sqlite3_stmt *check_stmt;
        int rc = sqlite3_prepare_v2(dbMan.db_, check_sql.c_str(), -1, &check_stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            Logger::error("Failed to prepare table check statement: " + std::string(sqlite3_errmsg(dbMan.db_)));
            return std::any(std::pair<bool, std::string>(false, "Failed to check table existence"));
        }

        sqlite3_bind_text(check_stmt, 1, captured_table_name.c_str(), -1, SQLITE_STATIC);
        rc = sqlite3_step(check_stmt);
        sqlite3_finalize(check_stmt);
        
        if (rc != SQLITE_ROW)
        {
            return std::any(std::pair<bool, std::string>(false, "Table does not exist: " + captured_table_name));
        }

        // Get all data from the table
        const std::string select_sql = "SELECT * FROM " + captured_table_name + " ORDER BY rowid";
        sqlite3_stmt *stmt;
        rc = sqlite3_prepare_v2(dbMan.db_, select_sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            Logger::error("Failed to prepare select statement: " + std::string(sqlite3_errmsg(dbMan.db_)));
            return std::any(std::pair<bool, std::string>(false, "Failed to prepare select statement"));
        }

        // Build a string representation of all table data
        std::stringstream table_data;
        int row_count = 0;
        
        while (sqlite3_step(stmt) == SQLITE_ROW)
        {
            row_count++;
            int columns = sqlite3_column_count(stmt);
            
            for (int i = 0; i < columns; i++)
            {
                if (i > 0) table_data << "|";
                
                int column_type = sqlite3_column_type(stmt, i);
                switch (column_type)
                {
                    case SQLITE_NULL:
                        table_data << "NULL";
                        break;
                    case SQLITE_INTEGER:
                        table_data << sqlite3_column_int64(stmt, i);
                        break;
                    case SQLITE_FLOAT:
                        table_data << sqlite3_column_double(stmt, i);
                        break;
                    case SQLITE_TEXT:
                        table_data << reinterpret_cast<const char *>(sqlite3_column_text(stmt, i));
                        break;
                    case SQLITE_BLOB:
                        // For BLOBs, we'll hash the binary data
                        const void *blob_data = sqlite3_column_blob(stmt, i);
                        int blob_size = sqlite3_column_bytes(stmt, i);
                        if (blob_data && blob_size > 0)
                        {
                            // Create a simple hash of the blob data
                            unsigned char hash[SHA256_DIGEST_LENGTH];
                            SHA256_CTX sha256;
                            SHA256_Init(&sha256);
                            SHA256_Update(&sha256, blob_data, blob_size);
                            SHA256_Final(hash, &sha256);
                            
                            std::stringstream blob_hash;
                            for (int j = 0; j < SHA256_DIGEST_LENGTH; j++)
                            {
                                blob_hash << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[j]);
                            }
                            table_data << "BLOB:" << blob_hash.str();
                        }
                        else
                        {
                            table_data << "BLOB:NULL";
                        }
                        break;
                }
            }
            table_data << "\n";
        }

        sqlite3_finalize(stmt);

        // Hash the table data
        std::string table_data_str = table_data.str();
        unsigned char hash[SHA256_DIGEST_LENGTH];
        SHA256_CTX sha256;
        SHA256_Init(&sha256);
        SHA256_Update(&sha256, table_data_str.c_str(), table_data_str.length());
        SHA256_Final(hash, &sha256);

        std::stringstream hash_ss;
        for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
        {
            hash_ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
        }

        Logger::debug("Generated hash for table " + captured_table_name + " with " + std::to_string(row_count) + " rows");
        return std::any(std::pair<bool, std::string>(true, hash_ss.str())); });

    // Wait for the result
    try
    {
        auto result = std::any_cast<std::pair<bool, std::string>>(future.get());
        return result;
    }
    catch (const std::exception &e)
    {
        Logger::error("Failed to get table hash: " + std::string(e.what()));
        return {false, "Failed to get table hash: " + std::string(e.what())};
    }
}

std::pair<bool, std::string> DatabaseManager::getDatabaseHash()
{
    Logger::debug("getDatabaseHash called");

    if (!waitForQueueInitialization())
    {
        Logger::error("Access queue not initialized after retries");
        return {false, "Access queue not initialized"};
    }

    // Enqueue the read operation
    auto future = access_queue_->enqueueRead([](DatabaseManager &dbMan)
                                             {
        Logger::debug("Executing getDatabaseHash in access queue");
        
        if (!dbMan.db_)
        {
            Logger::error("Database not initialized");
            return std::any(std::pair<bool, std::string>(false, "Database not initialized"));
        }
        
        // Get all table names
        const std::string tables_sql = "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name";
        sqlite3_stmt *tables_stmt;
        int rc = sqlite3_prepare_v2(dbMan.db_, tables_sql.c_str(), -1, &tables_stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            Logger::error("Failed to prepare tables select statement: " + std::string(sqlite3_errmsg(dbMan.db_)));
            return std::any(std::pair<bool, std::string>(false, "Failed to get table list"));
        }

        std::vector<std::string> table_names;
        while (sqlite3_step(tables_stmt) == SQLITE_ROW)
        {
            std::string table_name = reinterpret_cast<const char *>(sqlite3_column_text(tables_stmt, 0));
            table_names.push_back(table_name);
        }
        sqlite3_finalize(tables_stmt);

        // Build combined data from all tables
        std::stringstream combined_data;
        for (const auto &table_name : table_names)
        {
            combined_data << "TABLE:" << table_name << "\n";
            
            const std::string select_sql = "SELECT * FROM " + table_name + " ORDER BY rowid";
            sqlite3_stmt *stmt;
            rc = sqlite3_prepare_v2(dbMan.db_, select_sql.c_str(), -1, &stmt, nullptr);
            if (rc != SQLITE_OK)
            {
                Logger::error("Failed to prepare select statement for table " + table_name + ": " + std::string(sqlite3_errmsg(dbMan.db_)));
                continue;
            }

            int row_count = 0;
            while (sqlite3_step(stmt) == SQLITE_ROW)
            {
                row_count++;
                int columns = sqlite3_column_count(stmt);
                
                for (int i = 0; i < columns; i++)
                {
                    if (i > 0) combined_data << "|";
                    
                    int column_type = sqlite3_column_type(stmt, i);
                    switch (column_type)
                    {
                        case SQLITE_NULL:
                            combined_data << "NULL";
                            break;
                        case SQLITE_INTEGER:
                            combined_data << sqlite3_column_int64(stmt, i);
                            break;
                        case SQLITE_FLOAT:
                            combined_data << sqlite3_column_double(stmt, i);
                            break;
                        case SQLITE_TEXT:
                            combined_data << reinterpret_cast<const char *>(sqlite3_column_text(stmt, i));
                            break;
                        case SQLITE_BLOB:
                            // For BLOBs, we'll hash the binary data
                            const void *blob_data = sqlite3_column_blob(stmt, i);
                            int blob_size = sqlite3_column_bytes(stmt, i);
                            if (blob_data && blob_size > 0)
                            {
                                // Create a simple hash of the blob data
                                unsigned char hash[SHA256_DIGEST_LENGTH];
                                SHA256_CTX sha256;
                                SHA256_Init(&sha256);
                                SHA256_Update(&sha256, blob_data, blob_size);
                                SHA256_Final(hash, &sha256);
                                
                                std::stringstream blob_hash;
                                for (int j = 0; j < SHA256_DIGEST_LENGTH; j++)
                                {
                                    blob_hash << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[j]);
                                }
                                combined_data << "BLOB:" << blob_hash.str();
                            }
                            else
                            {
                                combined_data << "BLOB:NULL";
                            }
                            break;
                    }
                }
                combined_data << "\n";
            }
            sqlite3_finalize(stmt);
            combined_data << "END_TABLE:" << table_name << "\n";
        }

        // Hash the combined data
        std::string combined_data_str = combined_data.str();
        unsigned char hash[SHA256_DIGEST_LENGTH];
        SHA256_CTX sha256;
        SHA256_Init(&sha256);
        SHA256_Update(&sha256, combined_data_str.c_str(), combined_data_str.length());
        SHA256_Final(hash, &sha256);

        std::stringstream hash_ss;
        for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
        {
            hash_ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
        }

        Logger::debug("Generated database hash for " + std::to_string(table_names.size()) + " tables");
        return std::any(std::pair<bool, std::string>(true, hash_ss.str())); });

    // Wait for the result
    try
    {
        auto result = std::any_cast<std::pair<bool, std::string>>(future.get());
        return result;
    }
    catch (const std::exception &e)
    {
        Logger::error("Failed to get database hash: " + std::string(e.what()));
        return {false, "Failed to get database hash: " + std::string(e.what())};
    }
}

std::pair<bool, std::string> DatabaseManager::getDuplicateDetectionHash()
{
    Logger::debug("getDuplicateDetectionHash called");

    if (!waitForQueueInitialization())
    {
        Logger::error("Access queue not initialized after retries");
        return {false, "Access queue not initialized"};
    }

    // Enqueue the read operation
    auto future = access_queue_->enqueueRead([](DatabaseManager &dbMan)
                                             {
        Logger::debug("Executing getDuplicateDetectionHash in access queue");
        
        if (!dbMan.db_)
        {
            Logger::error("Database not initialized");
            return std::any(std::pair<bool, std::string>(false, "Database not initialized"));
        }
        
        // Get hashes for the three relevant tables
        std::vector<std::string> table_names = {"scanned_files", "cache_map", "media_processing_results"};
        std::vector<std::string> table_hashes;
        
        for (const auto &table_name : table_names)
        {
            // Check if table exists
            const std::string check_sql = "SELECT name FROM sqlite_master WHERE type='table' AND name=?";
            sqlite3_stmt *check_stmt;
            int rc = sqlite3_prepare_v2(dbMan.db_, check_sql.c_str(), -1, &check_stmt, nullptr);
            if (rc != SQLITE_OK)
            {
                Logger::error("Failed to prepare table check statement for " + table_name + ": " + std::string(sqlite3_errmsg(dbMan.db_)));
                return std::any(std::pair<bool, std::string>(false, "Failed to check table existence"));
            }

            sqlite3_bind_text(check_stmt, 1, table_name.c_str(), -1, SQLITE_STATIC);
            rc = sqlite3_step(check_stmt);
            sqlite3_finalize(check_stmt);
            
            if (rc != SQLITE_ROW)
            {
                // Table doesn't exist, use empty hash
                table_hashes.push_back("EMPTY_TABLE");
                continue;
            }

            // Get table data
            const std::string select_sql = "SELECT * FROM " + table_name + " ORDER BY rowid";
            sqlite3_stmt *stmt;
            rc = sqlite3_prepare_v2(dbMan.db_, select_sql.c_str(), -1, &stmt, nullptr);
            if (rc != SQLITE_OK)
            {
                Logger::error("Failed to prepare select statement for " + table_name + ": " + std::string(sqlite3_errmsg(dbMan.db_)));
                return std::any(std::pair<bool, std::string>(false, "Failed to prepare select statement"));
            }

            // Build string representation of table data
            std::stringstream table_data;
            int row_count = 0;
            
            while (sqlite3_step(stmt) == SQLITE_ROW)
            {
                row_count++;
                int columns = sqlite3_column_count(stmt);
                
                for (int i = 0; i < columns; i++)
                {
                    if (i > 0) table_data << "|";
                    
                    int column_type = sqlite3_column_type(stmt, i);
                    switch (column_type)
                    {
                        case SQLITE_NULL:
                            table_data << "NULL";
                            break;
                        case SQLITE_INTEGER:
                            table_data << sqlite3_column_int64(stmt, i);
                            break;
                        case SQLITE_FLOAT:
                            table_data << sqlite3_column_double(stmt, i);
                            break;
                        case SQLITE_TEXT:
                            table_data << reinterpret_cast<const char *>(sqlite3_column_text(stmt, i));
                            break;
                        case SQLITE_BLOB:
                            // For BLOBs, hash the binary data
                            const void *blob_data = sqlite3_column_blob(stmt, i);
                            int blob_size = sqlite3_column_bytes(stmt, i);
                            if (blob_data && blob_size > 0)
                            {
                                unsigned char hash[SHA256_DIGEST_LENGTH];
                                SHA256_CTX sha256;
                                SHA256_Init(&sha256);
                                SHA256_Update(&sha256, blob_data, blob_size);
                                SHA256_Final(hash, &sha256);
                                
                                std::stringstream blob_hash;
                                for (int j = 0; j < SHA256_DIGEST_LENGTH; j++)
                                {
                                    blob_hash << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[j]);
                                }
                                table_data << "BLOB:" << blob_hash.str();
                            }
                            else
                            {
                                table_data << "BLOB:NULL";
                            }
                            break;
                    }
                }
                table_data << "\n";
            }

            sqlite3_finalize(stmt);

            // Hash the table data
            std::string table_data_str = table_data.str();
            unsigned char hash[SHA256_DIGEST_LENGTH];
            SHA256_CTX sha256;
            SHA256_Init(&sha256);
            SHA256_Update(&sha256, table_data_str.c_str(), table_data_str.length());
            SHA256_Final(hash, &sha256);

            std::stringstream hash_ss;
            for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
            {
                hash_ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
            }

            table_hashes.push_back(hash_ss.str());
            Logger::debug("Generated hash for table " + table_name + " with " + std::to_string(row_count) + " rows");
        }

        // Combine the three table hashes with | delimiter
        std::string combined_data = table_hashes[0] + "|" + table_hashes[1] + "|" + table_hashes[2];
        
        // Hash the combined data
        unsigned char final_hash[SHA256_DIGEST_LENGTH];
        SHA256_CTX final_sha256;
        SHA256_Init(&final_sha256);
        SHA256_Update(&final_sha256, combined_data.c_str(), combined_data.length());
        SHA256_Final(final_hash, &final_sha256);

        std::stringstream final_hash_ss;
        for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
        {
            final_hash_ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(final_hash[i]);
        }

        Logger::debug("Generated combined duplicate detection hash for 3 tables");
        return std::any(std::pair<bool, std::string>(true, final_hash_ss.str())); });

    // Wait for the result
    try
    {
        auto result = std::any_cast<std::pair<bool, std::string>>(future.get());
        return result;
    }
    catch (const std::exception &e)
    {
        Logger::error("Failed to get duplicate detection hash: " + std::string(e.what()));
        return {false, "Failed to get duplicate detection hash: " + std::string(e.what())};
    }
}

std::string DatabaseManager::getTextFlag(const std::string &flag_name)
{
    if (!waitForQueueInitialization())
        return "";

    std::string value = "";
    auto future = access_queue_->enqueueRead([&flag_name, &value](DatabaseManager &dbMan)
                                             {
        if (!dbMan.db_) return std::any(std::string(""));
        const std::string select_sql = "SELECT value FROM flags WHERE name = ?";
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(dbMan.db_, select_sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
            return std::any(std::string(""));
        sqlite3_bind_text(stmt, 1, flag_name.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW)
        {
            value = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
        }
        sqlite3_finalize(stmt);
        return std::any(value); });
    try
    {
        value = std::any_cast<std::string>(future.get());
    }
    catch (...)
    {
        value = "";
    }
    return value;
}

DBOpResult DatabaseManager::setTextFlag(const std::string &flag_name, const std::string &value)
{
    if (!waitForQueueInitialization())
    {
        std::string msg = "Access queue not initialized after retries";
        Logger::error(msg);
        return DBOpResult(false, msg);
    }

    // Capture parameters for async execution
    std::string captured_flag_name = flag_name;
    std::string captured_value = value;
    std::string error_msg;
    bool success = true;

    // Enqueue the write operation
    access_queue_->enqueueWrite([captured_flag_name, captured_value, &error_msg, &success](DatabaseManager &dbMan)
                                {
        Logger::debug("Executing setTextFlag in write queue for: " + captured_flag_name);
        
        const std::string sql = "INSERT OR REPLACE INTO flags(name, value, updated_at) VALUES (?, ?, CURRENT_TIMESTAMP)";
        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare_v2(dbMan.db_, sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            error_msg = "Failed to prepare statement: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }

        sqlite3_bind_text(stmt, 1, captured_flag_name.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, captured_value.c_str(), -1, SQLITE_STATIC);

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE)
        {
            error_msg = "Failed to set text flag: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            success = false;
            return WriteOperationResult::Failure(error_msg);
        }

        Logger::debug("Set text flag: " + captured_flag_name + " = " + captured_value);
        return WriteOperationResult(); });

    waitForWrites();
    if (!success)
        return DBOpResult(false, error_msg);
    return DBOpResult(true);
}

DBOpResult DatabaseManager::resetAllProcessingFlagsOnStartup()
{
    Logger::info("Resetting all processing flags from -1 (in progress) to 0 (not processed) on startup");

    if (!waitForQueueInitialization())
    {
        std::string msg = "Access queue not initialized after retries";
        Logger::error(msg);
        return DBOpResult(false, msg);
    }

    std::string error_msg;
    bool success = true;

    // Enqueue the write operation
    auto operation_id = access_queue_->enqueueWrite([&error_msg, &success](DatabaseManager &dbMan)
                                                    {
        Logger::debug("Executing resetAllProcessingFlagsOnStartup in write queue");
        
        if (!dbMan.db_)
        {
            error_msg = "Database not initialized";
            Logger::error(error_msg);
            return WriteOperationResult::Failure(error_msg);
        }

        // Start transaction
        sqlite3_exec(dbMan.db_, "BEGIN TRANSACTION", nullptr, nullptr, nullptr);
        
        // Reset all processing flags from -1 to 0 for all modes
        const std::string update_sql = R"(
            UPDATE scanned_files 
            SET processed_fast = CASE WHEN processed_fast = -1 THEN 0 ELSE processed_fast END,
                processed_balanced = CASE WHEN processed_balanced = -1 THEN 0 ELSE processed_balanced END,
                processed_quality = CASE WHEN processed_quality = -1 THEN 0 ELSE processed_quality END
            WHERE processed_fast = -1 OR processed_balanced = -1 OR processed_quality = -1
        )";

        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare_v2(dbMan.db_, update_sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            error_msg = "Failed to prepare reset statement: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            sqlite3_exec(dbMan.db_, "ROLLBACK", nullptr, nullptr, nullptr);
            return WriteOperationResult::Failure(error_msg);
        }

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE)
        {
            error_msg = "Failed to reset processing flags: " + std::string(sqlite3_errmsg(dbMan.db_));
            Logger::error(error_msg);
            sqlite3_exec(dbMan.db_, "ROLLBACK", nullptr, nullptr, nullptr);
            return WriteOperationResult::Failure(error_msg);
        }

        // Get count of affected rows
        const std::string count_sql = "SELECT COUNT(*) FROM scanned_files WHERE processed_fast = -1 OR processed_balanced = -1 OR processed_quality = -1";
        sqlite3_stmt *count_stmt;
        rc = sqlite3_prepare_v2(dbMan.db_, count_sql.c_str(), -1, &count_stmt, nullptr);
        if (rc == SQLITE_OK)
        {
            if (sqlite3_step(count_stmt) == SQLITE_ROW)
            {
                int remaining_count = sqlite3_column_int(count_stmt, 0);
                if (remaining_count > 0)
                {
                    Logger::warn("Warning: " + std::to_string(remaining_count) + " files still have -1 status after reset");
                }
            }
            sqlite3_finalize(count_stmt);
        }

        // Commit transaction
        sqlite3_exec(dbMan.db_, "COMMIT", nullptr, nullptr, nullptr);
        
        Logger::info("Successfully reset all processing flags on startup");
        return WriteOperationResult(true); });

    waitForWrites();

    auto result = access_queue_->getOperationResult(operation_id);
    return DBOpResult(result.success, result.error_message);
}