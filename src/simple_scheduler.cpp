#include "core/simple_scheduler.hpp"
#include "core/server_config_manager.hpp"
#include "logging/logger.hpp"
#include <chrono>

SimpleScheduler::SimpleScheduler()
{
    Logger::info("SimpleScheduler constructor called");
    last_scan_time_ = std::chrono::system_clock::now();
    last_processing_time_ = std::chrono::system_clock::now();
}

SimpleScheduler::~SimpleScheduler()
{
    stop();
    Logger::info("SimpleScheduler destructor called");
}

SimpleScheduler &SimpleScheduler::getInstance()
{
    static SimpleScheduler instance;
    return instance;
}

void SimpleScheduler::start()
{
    if (running_.load())
    {
        Logger::warn("SimpleScheduler is already running");
        return;
    }

    running_.store(true);
    scheduler_thread_ = std::thread(&SimpleScheduler::schedulerLoop, this);
    Logger::info("SimpleScheduler started");
}

void SimpleScheduler::stop()
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

    Logger::info("SimpleScheduler stopped");
}

bool SimpleScheduler::isRunning() const
{
    return running_.load();
}

void SimpleScheduler::setScanCallback(std::function<void()> callback)
{
    scan_callback_ = callback;
}

void SimpleScheduler::setProcessingCallback(std::function<void()> callback)
{
    processing_callback_ = callback;
}

void SimpleScheduler::schedulerLoop()
{
    Logger::info("SimpleScheduler loop started");

    while (running_.load())
    {
        auto now = std::chrono::system_clock::now();
        auto &config_manager = ServerConfigManager::getInstance();

        // Check scan interval
        int scan_interval = config_manager.getScanIntervalSeconds();
        auto time_since_last_scan = std::chrono::duration_cast<std::chrono::seconds>(
                                        now - last_scan_time_)
                                        .count();

        if (time_since_last_scan >= scan_interval && scan_callback_)
        {
            Logger::info("Executing scheduled scan (interval: " + std::to_string(scan_interval) + "s)");
            try
            {
                scan_callback_();
                last_scan_time_ = now;
                Logger::info("Scheduled scan completed successfully");
            }
            catch (const std::exception &e)
            {
                Logger::error("Error during scheduled scan: " + std::string(e.what()));
            }
        }

        // Check processing interval
        int processing_interval = config_manager.getProcessingIntervalSeconds();
        auto time_since_last_processing = std::chrono::duration_cast<std::chrono::seconds>(
                                              now - last_processing_time_)
                                              .count();

        if (time_since_last_processing >= processing_interval && processing_callback_)
        {
            Logger::info("Executing scheduled processing (interval: " + std::to_string(processing_interval) + "s)");
            try
            {
                processing_callback_();
                last_processing_time_ = now;
                Logger::info("Scheduled processing completed successfully");
            }
            catch (const std::exception &e)
            {
                Logger::error("Error during scheduled processing: " + std::string(e.what()));
            }
        }

        // Sleep for a short interval before checking again
        std::this_thread::sleep_for(std::chrono::seconds(10));
    }

    Logger::info("SimpleScheduler loop ended");
}