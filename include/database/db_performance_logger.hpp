#pragma once

#include <string>
#include <chrono>
#include <vector>
#include <mutex>
#include <memory>
#include <fstream>
#include <atomic>

/**
 * @brief Database performance logger for tracing all database operations
 *
 * This logger tracks:
 * - Function name that made the database call
 * - SQL query executed
 * - Execution time
 * - Operation type (read/write)
 * - Queue wait time
 * - Total operation time
 */
class DatabasePerformanceLogger
{
public:
    static DatabasePerformanceLogger &getInstance();

    /**
     * @brief Log the start of a database operation
     * @param function_name Name of the function making the database call
     * @param sql_query SQL query being executed
     * @param operation_type Type of operation (read/write)
     * @return Operation ID for tracking
     */
    size_t logOperationStart(const std::string &function_name,
                             const std::string &sql_query,
                             const std::string &operation_type);

    /**
     * @brief Log the completion of a database operation
     * @param operation_id ID returned from logOperationStart
     * @param success Whether the operation succeeded
     * @param error_message Error message if operation failed
     */
    void logOperationComplete(size_t operation_id, bool success, const std::string &error_message = "");

    /**
     * @brief Log queue wait time for an operation
     * @param operation_id ID returned from logOperationStart
     * @param wait_time_ms Time spent waiting in queue
     */
    void logQueueWaitTime(size_t operation_id, int64_t wait_time_ms);

    /**
     * @brief Log database execution time for an operation
     * @param operation_id ID returned from logOperationStart
     * @param execution_time_ms Time spent executing the SQL query
     */
    void logExecutionTime(size_t operation_id, int64_t execution_time_ms);

    /**
     * @brief Get performance statistics
     * @return JSON string with performance metrics
     */
    std::string getPerformanceStats();

    /**
     * @brief Enable/disable performance logging
     * @param enabled Whether to enable logging
     */
    void setLoggingEnabled(bool enabled);

    /**
     * @brief Set log file path
     * @param file_path Path to the log file
     */
    void setLogFilePath(const std::string &file_path);

    /**
     * @brief Flush logs to file
     */
    void flushLogs();

    /**
     * @brief Clear all performance data
     */
    void clearStats();

private:
    DatabasePerformanceLogger();
    ~DatabasePerformanceLogger();

    // Prevent copying
    DatabasePerformanceLogger(const DatabasePerformanceLogger &) = delete;
    DatabasePerformanceLogger &operator=(const DatabasePerformanceLogger &) = delete;

    struct OperationRecord
    {
        size_t operation_id;
        std::string function_name;
        std::string sql_query;
        std::string operation_type;
        std::chrono::steady_clock::time_point start_time;
        std::chrono::steady_clock::time_point queue_start_time;
        std::chrono::steady_clock::time_point db_start_time;
        std::chrono::steady_clock::time_point end_time;
        int64_t queue_wait_time_ms;
        int64_t execution_time_ms;
        int64_t total_time_ms;
        bool completed;
        bool success;
        std::string error_message;
    };

    std::atomic<size_t> next_operation_id_;
    std::vector<std::shared_ptr<OperationRecord>> operations_;
    std::mutex operations_mutex_;
    std::atomic<bool> logging_enabled_;
    std::string log_file_path_;
    std::ofstream log_file_;
    std::mutex file_mutex_;

    void writeToLogFile(const std::string &message);
    std::string formatDuration(int64_t milliseconds);
    std::string sanitizeSqlQuery(const std::string &query);
};
