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

        void submit(const std::string &type, std::function<void()> fn);

        Status getStatus() const;

    private:
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

        // Config
        std::shared_ptr<UnifiedObservableConfigManager> cfg_;
        size_t effective_max_;
        std::chrono::milliseconds kill_timeout_;

        // Pool and bookkeeping
        Poco::ThreadPool pool_;
        mutable std::mutex mutex_;
        std::condition_variable cv_;
        std::atomic<bool> accepting_{true};
        std::atomic<bool> draining_{false};

        std::unordered_map<std::string, double> type_to_share_;
        std::unordered_map<std::string, size_t> type_to_running_;
        std::unordered_map<std::string, std::deque<std::function<void()>>> type_to_queue_;
        std::vector<std::string> round_robin_types_;
        size_t rr_index_ = 0;
        size_t running_total_ = 0;

        std::unordered_map<uint64_t, std::shared_ptr<FunctionRunnable>> active_runnables_;
        uint64_t next_id_ = 1;

        std::shared_ptr<FunctionRunnable> lockRunnable(uint64_t id);
    };
}
