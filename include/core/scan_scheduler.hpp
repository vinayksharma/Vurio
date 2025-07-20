#pragma once

#include <string>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <map>
#include <functional>
#include <nlohmann/json.hpp>
#include "logging/logger.hpp"

using json = nlohmann::json;

/**
 * @brief Represents a scheduled scan configuration
 */
struct ScanSchedule
{
    std::string id;                                 // Unique identifier for the schedule
    std::string directory;                          // Directory to scan
    int interval_seconds;                           // Scan interval in seconds
    bool recursive;                                 // Whether to scan recursively
    std::string database_path;                      // Database path for storing results
    bool enabled;                                   // Whether the schedule is active
    std::chrono::system_clock::time_point last_run; // Last execution time
    std::chrono::system_clock::time_point next_run; // Next scheduled execution time
};

/**
 * @brief Manages scheduled directory scans
 */
class ScanScheduler
{
public:
    // Singleton pattern
    static ScanScheduler &getInstance();

    // Schedule management
    std::string addSchedule(const std::string &directory, int interval_seconds,
                            bool recursive = true, const std::string &database_path = "scan_results.db");
    bool removeSchedule(const std::string &schedule_id);
    bool updateSchedule(const std::string &schedule_id, const ScanSchedule &new_config);
    bool enableSchedule(const std::string &schedule_id);
    bool disableSchedule(const std::string &schedule_id);

    // Schedule queries
    std::vector<ScanSchedule> getAllSchedules() const;
    ScanSchedule getSchedule(const std::string &schedule_id) const;
    bool scheduleExists(const std::string &schedule_id) const;

    // Control
    void start();
    void stop();
    bool isRunning() const;

    // Configuration
    void setScanCallback(std::function<void(const std::string &, bool, const std::string &)> callback);

    // Serialization
    json toJson() const;
    void fromJson(const json &j);

private:
    ScanScheduler();
    ~ScanScheduler();
    ScanScheduler(const ScanScheduler &) = delete;
    ScanScheduler &operator=(const ScanScheduler &) = delete;

    // Internal methods
    void schedulerLoop();
    void executeSchedule(const ScanSchedule &schedule);
    std::string generateScheduleId() const;
    void updateNextRunTime(ScanSchedule &schedule);

    // Member variables
    mutable std::mutex schedules_mutex_;
    std::map<std::string, ScanSchedule> schedules_;

    std::atomic<bool> running_{false};
    std::thread scheduler_thread_;

    std::function<void(const std::string &, bool, const std::string &)> scan_callback_;

    static std::atomic<int> next_schedule_id_;
};