#pragma once

#include <functional>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <memory>
#include <future>
#include <variant>
#include <any>
#include "database_manager.hpp"

class DatabaseManager;

struct WriteOperationResult
{
    bool success;
    std::string error_message;

    WriteOperationResult(bool s = true, const std::string &msg = "")
        : success(s), error_message(msg) {}

    static WriteOperationResult Failure(const std::string &msg = "")
    {
        return WriteOperationResult(false, msg);
    }
};

using WriteOperation = std::function<WriteOperationResult(DatabaseManager &)>;
using ReadOperation = std::function<std::any(DatabaseManager &)>;

class DatabaseAccessQueue
{
public:
    explicit DatabaseAccessQueue(DatabaseManager &db_manager);
    ~DatabaseAccessQueue();

    void enqueueWrite(WriteOperation operation);
    std::future<std::any> enqueueRead(ReadOperation operation);
    void wait_for_completion();
    void stop();

    // Get the result of the last completed operation
    WriteOperationResult getLastOperationResult() const;

private:
    void access_thread_worker();
    DatabaseManager &db_manager_;
    std::queue<std::variant<WriteOperation, std::pair<ReadOperation, std::promise<std::any>>>> operation_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::thread access_thread_;
    std::atomic<bool> is_running_;
    std::atomic<bool> should_stop_;

    // Track the last operation result
    mutable std::mutex result_mutex_;
    WriteOperationResult last_operation_result_;
};