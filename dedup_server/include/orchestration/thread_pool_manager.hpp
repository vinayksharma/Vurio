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
#include <unordered_set>
#include <vector>

#include "config/unified_observable_config.hpp"

namespace MediaDedup
{
    struct TaskCleanupHelper;

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
        virtual ~ThreadPoolManager(); // virtual: startOnPool is a test seam (CC-2)

        void initialize();
        void shutdownAndDrain(std::chrono::milliseconds killTimeout);
        void clearQueues();

        void setShare(const std::string &type, double share);
        void setTemporaryOverride(const std::string &type, double temporary_share);
        void clearTemporaryOverride(const std::string &type);

        void submit(const std::string &type, std::function<void()> fn, const std::string &file_path = "");
        bool canSubmit(const std::string &type, size_t max_queue_size = 10000) const;
        bool cancelTask(const std::string &type, const std::string &file_path);

        void pause() { paused_.store(true); }
        void resume();
        bool isPaused() const { return paused_.load(); }

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
        friend struct TaskCleanupHelper;

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
                             uint64_t id,
                             std::string file_path = "")
                : owner_(owner), type_(std::move(type)), fn_(std::move(fn)), id_(id), file_path_(std::move(file_path)) {}

            void run() override;

            uint64_t id() const { return id_; }
            const std::string &type() const { return type_; }
            const std::string &file_path() const { return file_path_; }

            /// CC-2: reclaim the callable from a runnable that was never
            /// started (pool_->start threw) so the task can be re-queued
            /// INTACT. Only valid before run() executes.
            std::function<void()> takeFn() { return std::move(fn_); }

        private:
            ThreadPoolManager &owner_;
            std::string type_;
            std::function<void()> fn_;
            uint64_t id_;
            std::string file_path_;
        };

    protected:
        /// CC-2 test seam: the single point where a task crosses into Poco.
        /// Tests override it to inject Poco::NoThreadAvailableException and
        /// exercise the re-queue path deterministically.
        virtual void startOnPool(Poco::Runnable &runnable) { pool_->start(runnable); }

    private:
        void onTaskStarted(const std::string &type, uint64_t id);
        void onTaskFinished(const std::string &type, uint64_t id);

        void schedule();
        size_t allowanceFor(const std::string &type) const;
        void onConfigChange(const ConfigChangeEvent &event);

        // Config
        std::shared_ptr<UnifiedObservableConfigManager> cfg_;
        size_t effective_max_;
        // Combined running-task cap for thumbnail_generator + preview_generator
        // (each task shells out to magick; unbounded bursts OOM the host).
        size_t media_generation_cap_ = 4;
        // CC-6: atomic — written by the config-monitor thread, read by
        // shutdownAndDrain and the dtor.
        std::atomic<long long> kill_timeout_ms_{10000};
        int idle_timeout_seconds_;

        // Pool and bookkeeping
        std::unique_ptr<Poco::ThreadPool> pool_;
        mutable std::mutex mutex_;
        std::condition_variable cv_;
        std::atomic<bool> accepting_{true};
        std::atomic<bool> draining_{false};
        std::atomic<bool> paused_{false};

        std::unordered_map<std::string, double> type_to_share_;
        std::unordered_map<std::string, double> type_to_temp_override_;
        std::unordered_map<std::string, size_t> type_to_running_;
        std::unordered_map<std::string, std::deque<QueuedTask>> type_to_queue_;
        // CC-6: O(1) dedup index over queued+running tasks that carry a
        // file_path (key: "type|path"). Replaces an O(queue)+O(active)
        // linear scan under mutex_ on EVERY submit.
        std::unordered_set<std::string> inflight_paths_;
        std::vector<std::string> round_robin_types_;
        size_t rr_index_ = 0;
        size_t running_total_ = 0;

        std::unordered_map<uint64_t, std::shared_ptr<FunctionRunnable>> active_runnables_;
        uint64_t next_id_ = 1;

        std::shared_ptr<FunctionRunnable> lockRunnable(uint64_t id);
    };
}
