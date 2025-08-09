#pragma once

#include <thread>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include "core/dedup_modes.hpp"

class DatabaseManager;

class DuplicateLinker
{
public:
    static DuplicateLinker &getInstance();

    void start(DatabaseManager &dbManager, int intervalSeconds = 30);
    void stop();
    void notifyNewResults();

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
};
