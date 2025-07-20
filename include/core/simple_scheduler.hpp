#pragma once

#include <string>
#include <chrono>
#include <thread>
#include <atomic>
#include <functional>
#include "logging/logger.hpp"

/**
 * @brief Simple scheduler that uses config intervals for scan and processing operations
 */
class SimpleScheduler
{
public:
    // Singleton pattern
    static SimpleScheduler &getInstance();

    // Control
    void start();
    void stop();
    bool isRunning() const;

    // Configuration
    void setScanCallback(std::function<void()> callback);
    void setProcessingCallback(std::function<void()> callback);

private:
    SimpleScheduler();
    ~SimpleScheduler();
    SimpleScheduler(const SimpleScheduler &) = delete;
    SimpleScheduler &operator=(const SimpleScheduler &) = delete;

    // Internal methods
    void schedulerLoop();

    // Member variables
    std::atomic<bool> running_{false};
    std::thread scheduler_thread_;

    std::function<void()> scan_callback_;
    std::function<void()> processing_callback_;

    std::chrono::system_clock::time_point last_scan_time_;
    std::chrono::system_clock::time_point last_processing_time_;
};