#include "database/db_performance_logger.hpp"
#include "logging/logger.hpp"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <filesystem>

DatabasePerformanceLogger::DatabasePerformanceLogger()
    : next_operation_id_(0), logging_enabled_(true), log_file_path_("db_performance.log")
{
    // Create log file
    setLogFilePath(log_file_path_);
}

DatabasePerformanceLogger::~DatabasePerformanceLogger()
{
    flushLogs();
    if (log_file_.is_open())
    {
        log_file_.close();
    }
}

DatabasePerformanceLogger &DatabasePerformanceLogger::getInstance()
{
    static DatabasePerformanceLogger instance;
    return instance;
}

size_t DatabasePerformanceLogger::logOperationStart(const std::string &function_name,
                                                    const std::string &sql_query,
                                                    const std::string &operation_type)
{
    if (!logging_enabled_)
    {
        return 0;
    }

    size_t operation_id = next_operation_id_.fetch_add(1);
    auto now = std::chrono::steady_clock::now();

    auto record = std::make_shared<OperationRecord>();
    record->operation_id = operation_id;
    record->function_name = function_name;
    record->sql_query = sanitizeSqlQuery(sql_query);
    record->operation_type = operation_type;
    record->start_time = now;
    record->queue_start_time = now;
    record->db_start_time = now;
    record->end_time = now;
    record->queue_wait_time_ms = 0;
    record->execution_time_ms = 0;
    record->total_time_ms = 0;
    record->completed = false;
    record->success = false;
    record->error_message = "";

    {
        std::lock_guard<std::mutex> lock(operations_mutex_);
        operations_.push_back(record);

        // Keep only last 10000 operations to prevent memory bloat
        if (operations_.size() > 10000)
        {
            operations_.erase(operations_.begin(), operations_.begin() + 1000);
        }
    }

    std::stringstream ss;
    ss << "DB_OP_START [" << operation_id << "] " << function_name
       << " (" << operation_type << ") - " << record->sql_query.substr(0, 100);
    if (record->sql_query.length() > 100)
    {
        ss << "...";
    }
    writeToLogFile(ss.str());

    return operation_id;
}

void DatabasePerformanceLogger::logOperationComplete(size_t operation_id, bool success, const std::string &error_message)
{
    if (!logging_enabled_)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(operations_mutex_);

    // Find the operation record
    auto it = std::find_if(operations_.begin(), operations_.end(),
                           [operation_id](const std::shared_ptr<OperationRecord> &op)
                           {
                               return op->operation_id == operation_id;
                           });

    if (it != operations_.end())
    {
        auto &record = *it;
        record->end_time = std::chrono::steady_clock::now();
        record->completed = true;
        record->success = success;
        record->error_message = error_message;

        // Calculate total time
        auto total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            record->end_time - record->start_time);
        record->total_time_ms = total_duration.count();

        std::stringstream ss;
        ss << "DB_OP_COMPLETE [" << operation_id << "] " << record->function_name
           << " - Success: " << (success ? "true" : "false")
           << ", Total Time: " << formatDuration(record->total_time_ms)
           << ", Queue Wait: " << formatDuration(record->queue_wait_time_ms)
           << ", Execution: " << formatDuration(record->execution_time_ms);

        if (!success && !error_message.empty())
        {
            ss << ", Error: " << error_message;
        }

        writeToLogFile(ss.str());
    }
}

void DatabasePerformanceLogger::logQueueWaitTime(size_t operation_id, int64_t wait_time_ms)
{
    if (!logging_enabled_)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(operations_mutex_);

    auto it = std::find_if(operations_.begin(), operations_.end(),
                           [operation_id](const std::shared_ptr<OperationRecord> &op)
                           {
                               return op->operation_id == operation_id;
                           });

    if (it != operations_.end())
    {
        auto &record = *it;
        record->queue_wait_time_ms = wait_time_ms;
        record->db_start_time = std::chrono::steady_clock::now();

        std::stringstream ss;
        ss << "DB_OP_QUEUE [" << operation_id << "] " << record->function_name
           << " - Queue Wait: " << formatDuration(wait_time_ms);
        writeToLogFile(ss.str());
    }
}

void DatabasePerformanceLogger::logExecutionTime(size_t operation_id, int64_t execution_time_ms)
{
    if (!logging_enabled_)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(operations_mutex_);

    auto it = std::find_if(operations_.begin(), operations_.end(),
                           [operation_id](const std::shared_ptr<OperationRecord> &op)
                           {
                               return op->operation_id == operation_id;
                           });

    if (it != operations_.end())
    {
        auto &record = *it;
        record->execution_time_ms = execution_time_ms;

        std::stringstream ss;
        ss << "DB_OP_EXEC [" << operation_id << "] " << record->function_name
           << " - Execution Time: " << formatDuration(execution_time_ms);
        writeToLogFile(ss.str());
    }
}

std::string DatabasePerformanceLogger::getPerformanceStats()
{
    std::lock_guard<std::mutex> lock(operations_mutex_);

    if (operations_.empty())
    {
        return "{\"error\": \"No performance data available\"}";
    }

    // Calculate statistics
    std::vector<int64_t> total_times;
    std::vector<int64_t> queue_wait_times;
    std::vector<int64_t> execution_times;

    for (const auto &op : operations_)
    {
        if (op->completed)
        {
            total_times.push_back(op->total_time_ms);
            queue_wait_times.push_back(op->queue_wait_time_ms);
            execution_times.push_back(op->execution_time_ms);
        }
    }

    if (total_times.empty())
    {
        return "{\"error\": \"No completed operations available\"}";
    }

    // Sort for percentiles
    std::sort(total_times.begin(), total_times.end());
    std::sort(queue_wait_times.begin(), queue_wait_times.end());
    std::sort(execution_times.begin(), execution_times.end());

    auto calculatePercentile = [](const std::vector<int64_t> &values, double percentile) -> int64_t
    {
        if (values.empty())
            return 0;
        size_t index = static_cast<size_t>(percentile * values.size() / 100.0);
        index = std::min(index, values.size() - 1);
        return values[index];
    };

    auto calculateAverage = [](const std::vector<int64_t> &values) -> double
    {
        if (values.empty())
            return 0.0;
        int64_t sum = 0;
        for (auto val : values)
            sum += val;
        return static_cast<double>(sum) / values.size();
    };

    std::stringstream ss;
    ss << "{"
       << "\"total_operations\": " << operations_.size() << ","
       << "\"completed_operations\": " << total_times.size() << ","
       << "\"total_time_ms\": {"
       << "\"avg\": " << std::fixed << std::setprecision(2) << calculateAverage(total_times) << ","
       << "\"min\": " << total_times.front() << ","
       << "\"max\": " << total_times.back() << ","
       << "\"p50\": " << calculatePercentile(total_times, 50) << ","
       << "\"p95\": " << calculatePercentile(total_times, 95) << ","
       << "\"p99\": " << calculatePercentile(total_times, 99)
       << "},"
       << "\"queue_wait_time_ms\": {"
       << "\"avg\": " << std::fixed << std::setprecision(2) << calculateAverage(queue_wait_times) << ","
       << "\"min\": " << queue_wait_times.front() << ","
       << "\"max\": " << queue_wait_times.back() << ","
       << "\"p50\": " << calculatePercentile(queue_wait_times, 50) << ","
       << "\"p95\": " << calculatePercentile(queue_wait_times, 95) << ","
       << "\"p99\": " << calculatePercentile(queue_wait_times, 99)
       << "},"
       << "\"execution_time_ms\": {"
       << "\"avg\": " << std::fixed << std::setprecision(2) << calculateAverage(execution_times) << ","
       << "\"min\": " << execution_times.front() << ","
       << "\"max\": " << execution_times.back() << ","
       << "\"p50\": " << calculatePercentile(execution_times, 50) << ","
       << "\"p95\": " << calculatePercentile(execution_times, 95) << ","
       << "\"p99\": " << calculatePercentile(execution_times, 99)
       << "}"
       << "}";

    return ss.str();
}

void DatabasePerformanceLogger::setLoggingEnabled(bool enabled)
{
    logging_enabled_ = enabled;
    std::stringstream ss;
    ss << "DB_PERF_LOGGING " << (enabled ? "ENABLED" : "DISABLED");
    writeToLogFile(ss.str());
}

void DatabasePerformanceLogger::setLogFilePath(const std::string &file_path)
{
    std::lock_guard<std::mutex> lock(file_mutex_);

    if (log_file_.is_open())
    {
        log_file_.close();
    }

    log_file_path_ = file_path;

    // Create directory if it doesn't exist
    std::filesystem::path path(file_path);
    if (path.has_parent_path())
    {
        std::filesystem::create_directories(path.parent_path());
    }

    log_file_.open(file_path, std::ios::app);
    if (log_file_.is_open())
    {
        std::stringstream ss;
        ss << "=== DB Performance Log Started at "
           << std::chrono::system_clock::now().time_since_epoch().count() << " ===";
        writeToLogFile(ss.str());
    }
}

void DatabasePerformanceLogger::flushLogs()
{
    std::lock_guard<std::mutex> lock(file_mutex_);
    if (log_file_.is_open())
    {
        log_file_.flush();
    }
}

void DatabasePerformanceLogger::clearStats()
{
    std::lock_guard<std::mutex> lock(operations_mutex_);
    operations_.clear();
    writeToLogFile("DB_PERF_STATS_CLEARED");
}

void DatabasePerformanceLogger::writeToLogFile(const std::string &message)
{
    std::lock_guard<std::mutex> lock(file_mutex_);

    if (log_file_.is_open())
    {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now.time_since_epoch()) %
                  1000;

        std::stringstream ss;
        ss << "[" << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S")
           << "." << std::setfill('0') << std::setw(3) << ms.count() << "] "
           << message << std::endl;

        log_file_ << ss.str();
        log_file_.flush();
    }

    // Also log to the main logger for visibility
    Logger::debug("[DB_PERF] " + message);
}

std::string DatabasePerformanceLogger::formatDuration(int64_t milliseconds)
{
    if (milliseconds < 1000)
    {
        return std::to_string(milliseconds) + "ms";
    }
    else if (milliseconds < 60000)
    {
        return std::to_string(milliseconds / 1000.0) + "s";
    }
    else
    {
        return std::to_string(milliseconds / 60000.0) + "m";
    }
}

std::string DatabasePerformanceLogger::sanitizeSqlQuery(const std::string &query)
{
    std::string sanitized = query;

    // Remove excessive whitespace
    std::string::iterator new_end = std::unique(sanitized.begin(), sanitized.end(),
                                                [](char a, char b)
                                                { return std::isspace(a) && std::isspace(b); });
    sanitized.erase(new_end, sanitized.end());

    // Truncate very long queries
    if (sanitized.length() > 500)
    {
        sanitized = sanitized.substr(0, 500) + "...";
    }

    return sanitized;
}
