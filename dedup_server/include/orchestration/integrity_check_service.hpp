#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace MediaDedup
{
    class DatabaseManager;
    class ThreadPoolManager;
    class UnifiedObservableConfigManager;

    namespace Orchestration
    {
        class SchedulerService;

        struct IntegrityCheckStatusSnapshot
        {
            bool is_running = false;
            int progress = 0;
            int total_files = 0;
            int processed_files = 0;
            int files_removed = 0;
            int groups_removed = 0;
            int thumbnails_removed = 0;
            int artifacts_removed = 0;
            int errors_removed = 0;
            std::string current_phase;
        };

        /**
         * @brief AR-1: the database integrity check as an orchestration
         * service.
         *
         * Previously this lived as static state + a runner on the HTTP
         * `DatabaseIntegrityCheckHandler` — the gRPC service reached UP into
         * core/web to start it and poll its status. Both surfaces now call
         * here; the HTTP handler is plumbing only.
         *
         * One check runs at a time (process-wide); `start` refuses re-entry.
         * The runner pauses the scheduler, drains the TPM (bounded, CC-5),
         * sweeps missing files / orphaned groups / orphaned metadata,
         * VACUUMs, resumes services, then triggers the media backfill hook.
         */
        class IntegrityCheckService
        {
        public:
            /// Starts the check on a detached (CC-5: lifetime-safe via real
            /// shared_ptr arguments) background thread. Returns false with
            /// out_error set when a check is already running.
            static bool start(std::shared_ptr<DatabaseManager> db,
                              std::shared_ptr<UnifiedObservableConfigManager> cfg,
                              std::shared_ptr<SchedulerService> scheduler,
                              std::shared_ptr<ThreadPoolManager> tpm,
                              std::string &out_error);

            static IntegrityCheckStatusSnapshot status();

        private:
            struct State
            {
                std::atomic<bool> is_running{false};
                std::atomic<int> progress{0};
                std::atomic<int> total_files{0};
                std::atomic<int> processed_files{0};
                std::atomic<int> files_removed{0};
                std::atomic<int> groups_removed{0};
                std::atomic<int> thumbnails_removed{0};
                std::atomic<int> artifacts_removed{0};
                std::atomic<int> errors_removed{0};
                std::string current_phase;
            };
            static State state_;
            static std::mutex state_mutex_;

            static void run(std::shared_ptr<DatabaseManager> db,
                            std::shared_ptr<UnifiedObservableConfigManager> cfg,
                            std::shared_ptr<SchedulerService> scheduler,
                            std::shared_ptr<ThreadPoolManager> tpm);
        };

    } // namespace Orchestration
} // namespace MediaDedup
