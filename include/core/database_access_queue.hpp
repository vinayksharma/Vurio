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
#include <map>
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
    /**
     * @brief Constructor
     * @param dbMan Reference to the DatabaseManager instance
     */
    explicit DatabaseAccessQueue(DatabaseManager &dbMan);
    ~DatabaseAccessQueue();

    size_t enqueueWrite(WriteOperation operation);
    std::future<std::any> enqueueRead(ReadOperation operation);
    void wait_for_completion();
    void stop();

    // Get the result of a specific operation by its ID
    WriteOperationResult getOperationResult(size_t operation_id) const;

    // Get the next operation ID for tracking
    size_t getNextOperationId() const { return next_operation_id_.load(); }

private:
    void access_thread_worker();
    DatabaseManager &db_manager_;
    std::queue<std::variant<std::pair<WriteOperation, size_t>, std::pair<ReadOperation, std::promise<std::any>>>> operation_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::thread access_thread_;
    std::atomic<bool> is_running_;
    std::atomic<bool> should_stop_;

    // Track operation results by ID
    mutable std::mutex results_mutex_;
    std::map<size_t, WriteOperationResult> operation_results_;
    std::atomic<size_t> next_operation_id_;
};