#pragma once

#include <thread>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include "core/dedup_modes.hpp"
#include "core/server_config_manager.hpp"

class DatabaseManager;

class DuplicateLinker : public ConfigObserver
{
public:
    static DuplicateLinker &getInstance();

    void start(DatabaseManager &dbManager, int intervalSeconds = 30);
    void stop();
    void notifyNewResults();
    // Request a full rescan of already processed files on next wake
    void requestFullRescan();

    /**
     * @brief Handle configuration changes (ConfigObserver implementation)
     * @param event Configuration change event
     */
    void onConfigChanged(const ConfigEvent &event) override;

private:
    DuplicateLinker() = default;
    ~DuplicateLinker() = default;
    DuplicateLinker(const DuplicateLinker &) = delete;
    DuplicateLinker &operator=(const DuplicateLinker &) = delete;

    void workerLoop();

    std::atomic<bool> running_{false};
    std::thread worker_;
    std::condition_variable cv_;
    std::mutex cv_mutex_;
    int interval_seconds_{30};
    DatabaseManager *db_{nullptr};
    long long last_seen_result_id_{0};
    std::atomic<bool> needs_full_rescan_{false};
    std::atomic<bool> full_pass_completed_{false};
    std::atomic<int> incremental_run_count_{0};
    static const int FULL_RESCAN_INTERVAL = 10; // Do full rescan every 10 incremental runs
};
