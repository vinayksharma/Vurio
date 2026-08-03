#pragma once

#include <chrono>
#include <condition_variable>
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

        // CC-4: enable_shared_from_this so job lambdas handed to the TPM can
        // capture a weak_ptr instead of a raw `this` (they can outlive the
        // scheduler through the shutdown drain window). Own instances through
        // std::shared_ptr — production and tests both do.
        class SchedulerService : public std::enable_shared_from_this<SchedulerService>
        {
        public:
            SchedulerService(std::shared_ptr<UnifiedObservableConfigManager> cfg,
                             std::shared_ptr<ThreadPoolManager> tpm);
            ~SchedulerService();

            void start();
            void stop();
            void pause();
            void resume();
            bool isPaused() const;

            void registerJob(const std::string &jobId,
                             std::chrono::milliseconds interval,
                             const std::string &typeKey,
                             std::function<void()> callback);

            void unregisterJob(const std::string &jobId);

            // Update job interval dynamically
            bool updateJobInterval(const std::string &jobId, std::chrono::milliseconds newInterval);

            // On-demand job execution
            bool triggerJob(const std::string &jobId);
            bool isJobRunning(const std::string &jobId);
            bool isRunning() const;
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

            // Set pause callback (called when scheduler is paused)
            void setPauseCallback(std::function<void()> callback);

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
                // CC-4: atomics — written by the worker (refreshJobConfig) and
                // read/written under jobsMutex_ by status/update calls; the
                // old plain chrono fields were a data race.
                std::atomic<long long> interval_ms{0};
                std::atomic<long long> backoff_ms{0};

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

            // Job execution and failure handling. CC-4: dispatch claims the
            // IDLE->RUNNING transition atomically (compare_exchange), so two
            // concurrent dispatch attempts can never double-run a job;
            // returns false when the job was already running.
            bool executeJobIfIdle(Job &job, const std::string &triggerType);
            void handleJobFailure(Job &job, const std::string &error);

            std::shared_ptr<UnifiedObservableConfigManager> cfg_;
            std::shared_ptr<ThreadPoolManager> tpm_;

            std::mutex jobsMutex_;
            std::unordered_map<std::string, std::unique_ptr<Job>> jobs_;

            std::atomic<bool> running_{false};
            std::atomic<bool> paused_{false};

            // CC-4: workers block here between runs and while paused instead
            // of 50 ms sleep-polling; stop/resume/unregister notify.
            std::mutex wakeMutex_;
            std::condition_variable wakeCv_;

            // CC-4: callbacks are set from the init thread and invoked from
            // worker/TPM threads — copied under this mutex before invoking.
            std::mutex callbackMutex_;
            std::function<void()> shutdownCallback_;
            std::function<void()> pauseCallback_;
        };
    } // namespace Orchestration
} // namespace MediaDedup
