#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <atomic>

#include "config/unified_observable_config.hpp"
#include "orchestration/thread_pool_manager.hpp"

namespace MediaDedup
{
    namespace Orchestration
    {
        class SchedulerService
        {
        public:
            SchedulerService(std::shared_ptr<UnifiedObservableConfigManager> cfg,
                             std::shared_ptr<ThreadPoolManager> tpm);
            ~SchedulerService();

            void start();
            void stop();

            void registerJob(const std::string &jobId,
                             std::chrono::milliseconds interval,
                             const std::string &typeKey,
                             std::function<void()> callback);

            void unregisterJob(const std::string &jobId);

        private:
            struct Job
            {
                std::string jobId;
                std::string typeKey;
                std::function<void()> callback;

                std::atomic<bool> stopFlag{false};
                std::thread worker;
                std::mutex mutex;
                std::chrono::milliseconds interval{0};

                // backoff state
                std::chrono::milliseconds backoff{0};
            };

            // configuration helpers (global defaults with per-job overrides)
            void refreshJobConfig(Job &job);
            std::chrono::milliseconds computeNextDelay(const Job &job, std::chrono::milliseconds baseInterval);

            std::shared_ptr<UnifiedObservableConfigManager> cfg_;
            std::shared_ptr<ThreadPoolManager> tpm_;

            std::mutex jobsMutex_;
            std::unordered_map<std::string, std::unique_ptr<Job>> jobs_;

            std::atomic<bool> running_{false};
        };
    } // namespace Orchestration
} // namespace MediaDedup
