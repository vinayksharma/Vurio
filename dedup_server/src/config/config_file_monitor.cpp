#include "config/config_file_monitor.hpp"
#include <filesystem>
#include <iostream>

namespace MediaDedup
{

    ConfigFileMonitor::ConfigFileMonitor(const std::string &file_path,
                                         std::chrono::milliseconds check_interval)
        : file_path_(file_path),
          check_interval_(check_interval),
          running_(false)
    {
        // Initialize last modification time
        try
        {
            if (std::filesystem::exists(file_path_))
            {
                auto file_time = std::filesystem::last_write_time(file_path_);
                // Convert file time to system time (simplified approach for C++17)
                auto duration = file_time.time_since_epoch();
                auto system_duration = std::chrono::duration_cast<std::chrono::system_clock::duration>(duration);
                last_modification_time_ = std::chrono::system_clock::time_point(system_duration);
            }
            else
            {
                last_modification_time_ = std::chrono::system_clock::time_point::min();
            }
        }
        catch (...)
        {
            last_modification_time_ = std::chrono::system_clock::time_point::min();
        }
    }

    ConfigFileMonitor::~ConfigFileMonitor()
    {
        stop();
    }

    bool ConfigFileMonitor::start()
    {
        if (running_)
        {
            return false; // Already running
        }

        running_ = true;
        monitor_thread_ = std::thread([this]()
                                      { monitoringLoop(); });
        return true;
    }

    bool ConfigFileMonitor::stop()
    {
        if (!running_)
        {
            return false; // Not running
        }

        running_ = false;
        if (monitor_thread_.joinable())
        {
            monitor_thread_.join();
        }
        return true;
    }

    bool ConfigFileMonitor::checkFileChanged()
    {
        return hasFileChanged();
    }

    bool ConfigFileMonitor::forceCheck()
    {
        if (hasFileChanged())
        {
            notifyFileChange(file_path_);
            return true;
        }
        return false;
    }

    void ConfigFileMonitor::monitoringLoop()
    {
        std::cout << "[ConfigFileMonitor] Starting file monitoring for: " << file_path_ << " (interval: " << check_interval_.count() << "ms)" << std::endl;
        
        while (running_)
        {
            if (hasFileChanged())
            {
                std::cout << "[ConfigFileMonitor] File changed detected: " << file_path_ << std::endl;
                notifyFileChange(file_path_);
                // Update the last modification time after detecting change
                try
                {
                    if (std::filesystem::exists(file_path_))
                    {
                        auto file_time = std::filesystem::last_write_time(file_path_);
                        // Convert file time to system time (simplified approach for C++17)
                        auto duration = file_time.time_since_epoch();
                        auto system_duration = std::chrono::duration_cast<std::chrono::system_clock::duration>(duration);
                        last_modification_time_ = std::chrono::system_clock::time_point(system_duration);
                    }
                }
                catch (...)
                {
                    // ignore
                }
            }
            std::this_thread::sleep_for(check_interval_);
        }
        
        std::cout << "[ConfigFileMonitor] File monitoring stopped for: " << file_path_ << std::endl;
    }

    bool ConfigFileMonitor::hasFileChanged() const
    {
        try
        {
            if (!std::filesystem::exists(file_path_))
            {
                return false;
            }

            auto file_time = std::filesystem::last_write_time(file_path_);
            // Convert file time to system time (simplified approach for C++17)
            auto duration = file_time.time_since_epoch();
            auto system_duration = std::chrono::duration_cast<std::chrono::system_clock::duration>(duration);
            auto current_time = std::chrono::system_clock::time_point(system_duration);

            return current_time > last_modification_time_;
        }
        catch (const std::exception &)
        {
            return false;
        }
    }

    void ConfigFileMonitor::notifyFileChange(const std::string &file_path)
    {
        if (file_change_callback_)
        {
            file_change_callback_(file_path);
        }
    }

} // namespace MediaDedup
