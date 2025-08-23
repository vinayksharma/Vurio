#include "database/database_access_queue.hpp"
#include "database/database_manager.hpp"
#include "logging/logger.hpp"
#include <chrono>

DatabaseAccessQueue::DatabaseAccessQueue(DatabaseManager &dbMan)
    : db_manager_(dbMan), next_operation_id_(0), pending_write_operations_(0)
{
    is_running_ = true;
    access_thread_ = std::thread(&DatabaseAccessQueue::access_thread_worker, this);
}

DatabaseAccessQueue::~DatabaseAccessQueue()
{
    stop();
    if (access_thread_.joinable())
    {
        access_thread_.join();
    }
    is_running_ = false;
}

size_t DatabaseAccessQueue::enqueueWrite(WriteOperation operation)
{
    Logger::debug("Enqueueing database write operation");
    size_t operation_id = next_operation_id_.fetch_add(1);
    pending_write_operations_.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        operation_queue_.push(std::make_pair(std::move(operation), operation_id));
    }
    queue_cv_.notify_one();
    return operation_id;
}

std::future<std::any> DatabaseAccessQueue::enqueueRead(ReadOperation operation)
{
    Logger::debug("Enqueueing database read operation");
    std::promise<std::any> promise;
    std::future<std::any> future = promise.get_future();

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        operation_queue_.push(std::make_pair(std::move(operation), std::move(promise)));
    }
    queue_cv_.notify_one();

    return future;
}

void DatabaseAccessQueue::wait_for_completion(std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(queue_mutex_);

    // First wait up to 10 seconds before showing any warning (reduced from 30)
    auto warning_threshold = std::chrono::milliseconds(10000);
    auto start_time = std::chrono::steady_clock::now();

    // Wait for operations to complete or timeout
    auto result = queue_cv_.wait_for(lock, timeout, [this]
                                     { return (operation_queue_.empty() && pending_write_operations_.load() == 0) || should_stop_; });

    if (!result)
    {
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (elapsed >= warning_threshold)
        {
            Logger::warn("Database access queue wait_for_completion timed out after " +
                         std::to_string(timeout.count()) + "ms - continuing to wait for operations to complete");
        }

        // Continue waiting indefinitely for operations to complete
        // Use a more robust condition check to avoid race conditions
        while (!should_stop_)
        {
            // Check if both conditions are met atomically
            if (operation_queue_.empty() && pending_write_operations_.load() == 0)
            {
                break;
            }

            // Wait for notification that something changed
            queue_cv_.wait(lock);
        }
    }
}

void DatabaseAccessQueue::stop()
{
    should_stop_ = true;
    queue_cv_.notify_all();
}

// Get the result of a specific operation by its ID
WriteOperationResult DatabaseAccessQueue::getOperationResult(size_t operation_id) const
{
    std::lock_guard<std::mutex> lock(results_mutex_);
    auto it = operation_results_.find(operation_id);
    if (it != operation_results_.end())
    {
        return it->second;
    }
    return WriteOperationResult::Failure("Operation not found");
}

bool DatabaseAccessQueue::checkLastOperationSuccess() const
{
    // For now, we'll assume success if we can reach this point
    // In a more sophisticated implementation, we could track the last operation result
    return true;
}

void DatabaseAccessQueue::access_thread_worker()
{
    while (true)
    {
        std::variant<std::pair<WriteOperation, size_t>, std::pair<ReadOperation, std::promise<std::any>>> operation;

        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this]
                           { return !operation_queue_.empty() || should_stop_; });

            if (should_stop_ && operation_queue_.empty())
            {
                break;
            }

            if (!operation_queue_.empty())
            {
                operation = std::move(operation_queue_.front());
                operation_queue_.pop();
            }
        }

        // Handle write operation
        if (std::holds_alternative<std::pair<WriteOperation, size_t>>(operation))
        {
            auto [write_op, operation_id] = std::get<std::pair<WriteOperation, size_t>>(std::move(operation));
            Logger::debug("Executing database write operation " + std::to_string(operation_id) + " in access thread");
            try
            {
                WriteOperationResult result = write_op(db_manager_);
                {
                    std::lock_guard<std::mutex> lock(results_mutex_);
                    operation_results_[operation_id] = result;
                }
                Logger::debug("Database write operation " + std::to_string(operation_id) + " completed successfully");
            }
            catch (const std::exception &e)
            {
                Logger::error("Database write operation " + std::to_string(operation_id) + " failed: " + std::string(e.what()));
                {
                    std::lock_guard<std::mutex> lock(results_mutex_);
                    operation_results_[operation_id] = WriteOperationResult::Failure(e.what());
                }
            }

            // Decrement pending write operations counter
            pending_write_operations_.fetch_sub(1);
        }
        // Handle read operation
        else if (std::holds_alternative<std::pair<ReadOperation, std::promise<std::any>>>(operation))
        {
            auto [read_op, promise] = std::get<std::pair<ReadOperation, std::promise<std::any>>>(std::move(operation));
            Logger::debug("Executing database read operation in access thread");
            try
            {
                std::any result = read_op(db_manager_);
                promise.set_value(std::move(result));
                Logger::debug("Database read operation completed successfully");
            }
            catch (const std::exception &e)
            {
                Logger::error("Database read operation failed: " + std::string(e.what()));
                promise.set_exception(std::current_exception());
            }
        }

        // Notify waiters if the queue is now empty and all write operations are complete
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (operation_queue_.empty() && pending_write_operations_.load() == 0)
            {
                queue_cv_.notify_all();
            }
            // Also notify when any operation completes to wake up waiters
            else
            {
                queue_cv_.notify_all();
            }
        }
    }
}