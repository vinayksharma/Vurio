#include "core/database_access_queue.hpp"
#include "logging/logger.hpp"

DatabaseAccessQueue::DatabaseAccessQueue(DatabaseManager &db_manager)
    : db_manager_(db_manager)
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

void DatabaseAccessQueue::enqueueWrite(WriteOperation operation)
{
    Logger::debug("Enqueueing database write operation");
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        operation_queue_.push(std::move(operation));
    }
    queue_cv_.notify_one();
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

void DatabaseAccessQueue::wait_for_completion()
{
    std::unique_lock<std::mutex> lock(queue_mutex_);
    queue_cv_.wait(lock, [this]
                   { return operation_queue_.empty() || should_stop_; });
}

void DatabaseAccessQueue::stop()
{
    should_stop_ = true;
    queue_cv_.notify_all();
}

// Get the result of the last completed operation
WriteOperationResult DatabaseAccessQueue::getLastOperationResult() const
{
    std::lock_guard<std::mutex> lock(result_mutex_);
    return last_operation_result_;
}

void DatabaseAccessQueue::access_thread_worker()
{
    while (true)
    {
        std::variant<WriteOperation, std::pair<ReadOperation, std::promise<std::any>>> operation;

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
        if (std::holds_alternative<WriteOperation>(operation))
        {
            WriteOperation write_op = std::get<WriteOperation>(std::move(operation));
            Logger::debug("Executing database write operation in access thread");
            try
            {
                WriteOperationResult result = write_op(db_manager_);
                {
                    std::lock_guard<std::mutex> lock(result_mutex_);
                    last_operation_result_ = result;
                }
                Logger::debug("Database write operation completed successfully");
            }
            catch (const std::exception &e)
            {
                Logger::error("Database write operation failed: " + std::string(e.what()));
                {
                    std::lock_guard<std::mutex> lock(result_mutex_);
                    last_operation_result_ = WriteOperationResult::Failure();
                }
            }
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

        // Notify waiters if the queue is now empty
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (operation_queue_.empty())
            {
                queue_cv_.notify_all();
            }
        }
    }
}