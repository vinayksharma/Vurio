#pragma once

#include <Poco/ThreadPool.h>
#include <Poco/Runnable.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "config/unified_observable_config.hpp"

namespace MediaDedup
{

    class ThreadPoolManager
    {
    public:
        struct TypeStatus
        {
            double share = 1.0;
            size_t running = 0;
            size_t queued = 0;
        };

        struct Status
        {
            size_t effectiveMax = 0;
            size_t runningTotal = 0;
            std::unordered_map<std::string, TypeStatus> perType;
        };

        explicit ThreadPoolManager(std::shared_ptr<UnifiedObservableConfigManager> cfg);
        ~ThreadPoolManager();

        void initialize();
        void shutdownAndDrain(std::chrono::milliseconds killTimeout);

        void setShare(const std::string &type, double share);

        void submit(const std::string &type, std::function<void()> fn, const std::string &file_path = "");
        bool canSubmit(const std::string &type, size_t max_queue_size = 10000) const;

        Status getStatus() const;

        /**
         * @brief Get current queue depth for a specific task type
         * @param type Task type (e.g., "image_processor", "video_processor", "audio_processor")
         * @return Current number of queued tasks for the specified type
         */
        size_t getQueueDepth(const std::string &type) const;

        /**
         * @brief Get current queue depths for all task types
         * @return Map of task type to current queue depth
         */
        std::unordered_map<std::string, size_t> getAllQueueDepths() const;

        /**
         * @brief Get pending file paths for a specific task type
         * @param type Task type (e.g., "media_processor")
         * @return Vector of file paths currently queued for the specified type
         */
        std::vector<std::string> getPendingFilePaths(const std::string &type) const;

        /**
         * @brief Get pending file paths for all task types
         * @return Map of task type to vector of pending file paths
         */
        std::unordered_map<std::string, std::vector<std::string>> getAllPendingFilePaths() const;

    private:
        struct QueuedTask
        {
            std::function<void()> fn;
            std::string file_path;
            
            QueuedTask(std::function<void()> f, const std::string& path = "")
                : fn(std::move(f)), file_path(path) {}
        };

        class FunctionRunnable : public Poco::Runnable
        {
        public:
            FunctionRunnable(ThreadPoolManager &owner,
                             std::string type,
                             std::function<void()> fn,
                             uint64_t id)
                : owner_(owner), type_(std::move(type)), fn_(std::move(fn)), id_(id) {}

            void run() override;

            uint64_t id() const { return id_; }
            const std::string &type() const { return type_; }

        private:
            ThreadPoolManager &owner_;
            std::string type_;
            std::function<void()> fn_;
            uint64_t id_;
        };

        void onTaskStarted(const std::string &type, uint64_t id);
        void onTaskFinished(const std::string &type, uint64_t id);

        void schedule();
        size_t allowanceFor(const std::string &type) const;
        void onConfigChange(const ConfigChangeEvent &event);
        void recreateThreadPool();

        // Config
        std::shared_ptr<UnifiedObservableConfigManager> cfg_;
        size_t effective_max_;
        std::chrono::milliseconds kill_timeout_;
        int idle_timeout_seconds_;

        // Pool and bookkeeping
        std::unique_ptr<Poco::ThreadPool> pool_;
        mutable std::mutex mutex_;
        std::condition_variable cv_;
        std::atomic<bool> accepting_{true};
        std::atomic<bool> draining_{false};

        std::unordered_map<std::string, double> type_to_share_;
        std::unordered_map<std::string, size_t> type_to_running_;
        std::unordered_map<std::string, std::deque<QueuedTask>> type_to_queue_;
        std::vector<std::string> round_robin_types_;
        size_t rr_index_ = 0;
        size_t running_total_ = 0;

        std::unordered_map<uint64_t, std::shared_ptr<FunctionRunnable>> active_runnables_;
        uint64_t next_id_ = 1;

        std::shared_ptr<FunctionRunnable> lockRunnable(uint64_t id);
    };
}
