#pragma once

#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <thread>
#include <atomic>
#include <memory>
#include "database_manager.hpp"

class DatabaseWriteQueue
{
public:
    using WriteOperation = std::function<void(DatabaseManager &)>;

    explicit DatabaseWriteQueue(DatabaseManager &db_manager);
    ~DatabaseWriteQueue();

    // Enqueue a write operation to be executed by the write thread
    void enqueue(WriteOperation operation);

    // Wait for all pending operations to complete
    void wait_for_completion();

    // Stop the write queue thread
    void stop();

private:
    void write_thread_worker();

    DatabaseManager &db_manager_;
    std::queue<WriteOperation> write_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::thread write_thread_;
    std::atomic<bool> should_stop_{false};
    std::atomic<bool> is_running_{false};
};