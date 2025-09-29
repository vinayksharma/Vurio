#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <atomic>
#include <vector>

#include "config/unified_observable_config.hpp"
#include "orchestration/thread_pool_manager.hpp"

namespace MediaDedup
{
    namespace Orchestration
    {
        // Forward declaration for shutdown callback
        class ConsoleInputManager;

        enum class JobState
        {
            IDLE,   // Not currently running
            RUNNING // Currently executing
            // No FAILED state - jobs don't remember failures
        };

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

            // On-demand job execution
            bool triggerJob(const std::string &jobId);
            bool isJobRunning(const std::string &jobId);
            std::vector<std::string> listJobs();

            // Job status for monitoring
            struct JobStatus
            {
                std::string jobId;
                JobState state;
                std::chrono::milliseconds interval;
                std::chrono::steady_clock::time_point lastRun;
                std::chrono::steady_clock::time_point nextRun;
                int consecutiveFailures;
                int totalFailures;
            };
            std::vector<JobStatus> getJobStatuses();

            // Set shutdown callback (called when max failures reached)
            void setShutdownCallback(std::function<void()> callback);

        private:
            struct Job
            {
                std::string jobId;
                std::string typeKey;
                std::function<void()> callback;

                // Existing fields
                std::atomic<bool> stopFlag{false};
                std::thread worker;
                std::mutex mutex;
                std::chrono::milliseconds interval{0};
                std::chrono::milliseconds backoff{0};

                // NEW: Enhanced state tracking
                std::atomic<JobState> state{JobState::IDLE};
                std::atomic<int> consecutiveFailures{0};
                std::atomic<int> totalFailures{0};
                std::chrono::steady_clock::time_point lastRun;
                std::chrono::steady_clock::time_point nextRun;
                int maxConsecutiveFailures{5}; // Loaded at registration
            };

            // configuration helpers (global defaults with per-job overrides)
            void refreshJobConfig(Job &job);
            std::chrono::milliseconds computeNextDelay(const Job &job, std::chrono::milliseconds baseInterval);

            // Job execution and failure handling
            void executeJob(Job &job, const std::string &triggerType);
            void handleJobFailure(Job &job, const std::string &error);

            std::shared_ptr<UnifiedObservableConfigManager> cfg_;
            std::shared_ptr<ThreadPoolManager> tpm_;

            std::mutex jobsMutex_;
            std::unordered_map<std::string, std::unique_ptr<Job>> jobs_;

            std::atomic<bool> running_{false};
            std::function<void()> shutdownCallback_;
        };
    } // namespace Orchestration
} // namespace MediaDedup
