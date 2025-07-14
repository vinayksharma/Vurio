#include "core/database_write_queue.hpp"
#include "logging/logger.hpp"

DatabaseWriteQueue::DatabaseWriteQueue(DatabaseManager &db_manager)
    : db_manager_(db_manager)
{
    is_running_ = true;
    write_thread_ = std::thread(&DatabaseWriteQueue::write_thread_worker, this);
}

DatabaseWriteQueue::~DatabaseWriteQueue()
{
    stop();
    if (write_thread_.joinable())
    {
        write_thread_.join();
    }
    is_running_ = false;
}

void DatabaseWriteQueue::enqueue(WriteOperation operation)
{
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        write_queue_.push(std::move(operation));
    }
    queue_cv_.notify_one();
}

void DatabaseWriteQueue::wait_for_completion()
{
    std::unique_lock<std::mutex> lock(queue_mutex_);
    queue_cv_.wait(lock, [this]
                   { return write_queue_.empty() || should_stop_; });
}

void DatabaseWriteQueue::stop()
{
    should_stop_ = true;
    queue_cv_.notify_all();
}

void DatabaseWriteQueue::write_thread_worker()
{
    while (true)
    {
        WriteOperation operation;

        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this]
                           { return !write_queue_.empty() || should_stop_; });

            if (should_stop_ && write_queue_.empty())
            {
                break;
            }

            if (!write_queue_.empty())
            {
                operation = std::move(write_queue_.front());
                write_queue_.pop();
            }
        }

        if (operation)
        {
            try
            {
                operation(db_manager_);
            }
            catch (const std::exception &e)
            {
                Logger::error("Database write operation failed: " + std::string(e.what()));
            }
        }

        // Notify waiters if the queue is now empty
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (write_queue_.empty())
            {
                queue_cv_.notify_all();
            }
        }
    }
}