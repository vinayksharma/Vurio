#include "core/scan_scheduler.hpp"
#include "core/file_utils.hpp"
#include "database/database_manager.hpp"
#include <algorithm>
#include <random>
#include <sstream>
#include <iomanip>

std::atomic<int> ScanScheduler::next_schedule_id_{1};

ScanScheduler::ScanScheduler()
{
    Logger::info("ScanScheduler constructor called");
}

ScanScheduler::~ScanScheduler()
{
    stop();
    Logger::info("ScanScheduler destructor called");
}

ScanScheduler &ScanScheduler::getInstance()
{
    static ScanScheduler instance;
    return instance;
}

std::string ScanScheduler::addSchedule(const std::string &directory, int interval_seconds,
                                       bool recursive, const std::string &database_path)
{
    std::lock_guard<std::mutex> lock(schedules_mutex_);

    std::string schedule_id = generateScheduleId();

    ScanSchedule schedule;
    schedule.id = schedule_id;
    schedule.directory = directory;
    schedule.interval_seconds = interval_seconds;
    schedule.recursive = recursive;
    schedule.database_path = database_path;
    schedule.enabled = true;
    schedule.last_run = std::chrono::system_clock::time_point::min();
    updateNextRunTime(schedule);

    schedules_[schedule_id] = schedule;

    Logger::info("Added scan schedule: " + schedule_id + " for directory: " + directory +
                 " (interval: " + std::to_string(interval_seconds) + "s)");

    return schedule_id;
}

bool ScanScheduler::removeSchedule(const std::string &schedule_id)
{
    std::lock_guard<std::mutex> lock(schedules_mutex_);

    auto it = schedules_.find(schedule_id);
    if (it == schedules_.end())
    {
        Logger::warn("Attempted to remove non-existent schedule: " + schedule_id);
        return false;
    }

    schedules_.erase(it);
    Logger::info("Removed scan schedule: " + schedule_id);
    return true;
}

bool ScanScheduler::updateSchedule(const std::string &schedule_id, const ScanSchedule &new_config)
{
    std::lock_guard<std::mutex> lock(schedules_mutex_);

    auto it = schedules_.find(schedule_id);
    if (it == schedules_.end())
    {
        Logger::warn("Attempted to update non-existent schedule: " + schedule_id);
        return false;
    }

    ScanSchedule &existing = it->second;
    existing.directory = new_config.directory;
    existing.interval_seconds = new_config.interval_seconds;
    existing.recursive = new_config.recursive;
    existing.database_path = new_config.database_path;
    existing.enabled = new_config.enabled;
    updateNextRunTime(existing);

    Logger::info("Updated scan schedule: " + schedule_id);
    return true;
}

bool ScanScheduler::enableSchedule(const std::string &schedule_id)
{
    std::lock_guard<std::mutex> lock(schedules_mutex_);

    auto it = schedules_.find(schedule_id);
    if (it == schedules_.end())
    {
        Logger::warn("Attempted to enable non-existent schedule: " + schedule_id);
        return false;
    }

    it->second.enabled = true;
    updateNextRunTime(it->second);
    Logger::info("Enabled scan schedule: " + schedule_id);
    return true;
}

bool ScanScheduler::disableSchedule(const std::string &schedule_id)
{
    std::lock_guard<std::mutex> lock(schedules_mutex_);

    auto it = schedules_.find(schedule_id);
    if (it == schedules_.end())
    {
        Logger::warn("Attempted to disable non-existent schedule: " + schedule_id);
        return false;
    }

    it->second.enabled = false;
    Logger::info("Disabled scan schedule: " + schedule_id);
    return true;
}

std::vector<ScanSchedule> ScanScheduler::getAllSchedules() const
{
    std::lock_guard<std::mutex> lock(schedules_mutex_);

    std::vector<ScanSchedule> result;
    result.reserve(schedules_.size());

    for (const auto &[id, schedule] : schedules_)
    {
        result.push_back(schedule);
    }

    return result;
}

ScanSchedule ScanScheduler::getSchedule(const std::string &schedule_id) const
{
    std::lock_guard<std::mutex> lock(schedules_mutex_);

    auto it = schedules_.find(schedule_id);
    if (it == schedules_.end())
    {
        throw std::runtime_error("Schedule not found: " + schedule_id);
    }

    return it->second;
}

bool ScanScheduler::scheduleExists(const std::string &schedule_id) const
{
    std::lock_guard<std::mutex> lock(schedules_mutex_);
    return schedules_.find(schedule_id) != schedules_.end();
}

void ScanScheduler::start()
{
    if (running_.load())
    {
        Logger::warn("ScanScheduler is already running");
        return;
    }

    running_.store(true);
    scheduler_thread_ = std::thread(&ScanScheduler::schedulerLoop, this);
    Logger::info("ScanScheduler started");
}

void ScanScheduler::stop()
{
    if (!running_.load())
    {
        return;
    }

    running_.store(false);

    if (scheduler_thread_.joinable())
    {
        scheduler_thread_.join();
    }

    Logger::info("ScanScheduler stopped");
}

bool ScanScheduler::isRunning() const
{
    return running_.load();
}

void ScanScheduler::setScanCallback(std::function<void(const std::string &, bool, const std::string &)> callback)
{
    scan_callback_ = callback;
}

void ScanScheduler::schedulerLoop()
{
    Logger::info("ScanScheduler loop started");

    while (running_.load())
    {
        auto now = std::chrono::system_clock::now();
        std::vector<std::string> schedules_to_run;

        // Find schedules that need to run
        {
            std::lock_guard<std::mutex> lock(schedules_mutex_);
            for (const auto &[id, schedule] : schedules_)
            {
                if (schedule.enabled && now >= schedule.next_run)
                {
                    schedules_to_run.push_back(id);
                }
            }
        }

        // Execute schedules that are due
        for (const auto &schedule_id : schedules_to_run)
        {
            try
            {
                ScanSchedule schedule = getSchedule(schedule_id);
                executeSchedule(schedule);
            }
            catch (const std::exception &e)
            {
                Logger::error("Error executing schedule " + schedule_id + ": " + e.what());
            }
        }

        // Sleep for a short interval before checking again
        std::this_thread::sleep_for(std::chrono::seconds(10));
    }

    Logger::info("ScanScheduler loop ended");
}

void ScanScheduler::executeSchedule(const ScanSchedule &schedule)
{
    Logger::info("Executing scheduled scan: " + schedule.id + " for directory: " + schedule.directory);

    // Update last run time
    {
        std::lock_guard<std::mutex> lock(schedules_mutex_);
        auto it = schedules_.find(schedule.id);
        if (it != schedules_.end())
        {
            it->second.last_run = std::chrono::system_clock::now();
            updateNextRunTime(it->second);
        }
    }

    // Execute the scan
    if (scan_callback_)
    {
        scan_callback_(schedule.directory, schedule.recursive, schedule.database_path);
    }
    else
    {
        // Fallback: perform basic scan
        try
        {
            DatabaseManager &db_manager = DatabaseManager::getInstance(schedule.database_path);

            // Store the scan path in user inputs
            db_manager.storeUserInput("scan_path", schedule.directory);

            // Perform the scan
            auto observable = FileUtils::listFilesAsObservable(schedule.directory, schedule.recursive);

            size_t files_scanned = 0;
            observable.subscribe(
                [&](const std::string &file_path)
                {
                    try
                    {
                        auto result = db_manager.storeScannedFile(file_path);
                        if (result.success)
                        {
                            files_scanned++;
                        }
                    }
                    catch (const std::exception &e)
                    {
                        Logger::error("Error processing file in scheduled scan: " + std::string(e.what()));
                    }
                },
                [](const std::exception &e)
                {
                    Logger::error("Scheduled scan error: " + std::string(e.what()));
                },
                [&]()
                {
                    Logger::info("Scheduled scan completed: " + schedule.id +
                                 " - Files scanned: " + std::to_string(files_scanned));
                });
        }
        catch (const std::exception &e)
        {
            Logger::error("Error in scheduled scan execution: " + std::string(e.what()));
        }
    }
}

std::string ScanScheduler::generateScheduleId() const
{
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(1000, 9999);

    std::ostringstream oss;
    oss << "schedule_" << std::setfill('0') << std::setw(4) << dis(gen);
    return oss.str();
}

void ScanScheduler::updateNextRunTime(ScanSchedule &schedule)
{
    auto now = std::chrono::system_clock::now();
    schedule.next_run = now + std::chrono::seconds(schedule.interval_seconds);
}

json ScanScheduler::toJson() const
{
    std::lock_guard<std::mutex> lock(schedules_mutex_);

    json j = json::array();
    for (const auto &[id, schedule] : schedules_)
    {
        json schedule_json;
        schedule_json["id"] = schedule.id;
        schedule_json["directory"] = schedule.directory;
        schedule_json["interval_seconds"] = schedule.interval_seconds;
        schedule_json["recursive"] = schedule.recursive;
        schedule_json["database_path"] = schedule.database_path;
        schedule_json["enabled"] = schedule.enabled;

        auto last_run_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               schedule.last_run.time_since_epoch())
                               .count();
        schedule_json["last_run"] = last_run_ms;

        auto next_run_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               schedule.next_run.time_since_epoch())
                               .count();
        schedule_json["next_run"] = next_run_ms;

        j.push_back(schedule_json);
    }

    return j;
}

void ScanScheduler::fromJson(const json &j)
{
    std::lock_guard<std::mutex> lock(schedules_mutex_);

    schedules_.clear();

    if (!j.is_array())
    {
        Logger::warn("Invalid JSON format for scan schedules");
        return;
    }

    for (const auto &schedule_json : j)
    {
        ScanSchedule schedule;
        schedule.id = schedule_json["id"];
        schedule.directory = schedule_json["directory"];
        schedule.interval_seconds = schedule_json["interval_seconds"];
        schedule.recursive = schedule_json["recursive"];
        schedule.database_path = schedule_json["database_path"];
        schedule.enabled = schedule_json["enabled"];

        auto last_run_ms = schedule_json["last_run"].get<int64_t>();
        schedule.last_run = std::chrono::system_clock::time_point(
            std::chrono::milliseconds(last_run_ms));

        auto next_run_ms = schedule_json["next_run"].get<int64_t>();
        schedule.next_run = std::chrono::system_clock::time_point(
            std::chrono::milliseconds(next_run_ms));

        schedules_[schedule.id] = schedule;
    }

    Logger::info("Loaded " + std::to_string(schedules_.size()) + " scan schedules from JSON");
}