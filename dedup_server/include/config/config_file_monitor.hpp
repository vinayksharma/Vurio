#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>

namespace MediaDedup
{

    /**
     * @brief File change callback type
     */
    using FileChangeCallback = std::function<void(const std::string &)>;

    /**
     * @brief Manages file monitoring for configuration files
     *
     * Handles background file monitoring with configurable intervals
     * and proper thread lifecycle management.
     */
    class ConfigFileMonitor
    {
    public:
        /**
         * @brief Constructor
         * @param file_path Path to the file to monitor
         * @param check_interval Interval between file checks
         */
        ConfigFileMonitor(const std::string &file_path,
                          std::chrono::milliseconds check_interval = std::chrono::milliseconds(1000));

        /**
         * @brief Destructor
         */
        ~ConfigFileMonitor();

        // Monitoring control
        /**
         * @brief Start file monitoring
         * @return true if started successfully, false if already running
         */
        bool start();

        /**
         * @brief Stop file monitoring
         * @return true if stopped successfully, false if not running
         */
        bool stop();

        /**
         * @brief Check if monitoring is active
         * @return true if monitoring is running, false otherwise
         */
        bool isRunning() const { return running_; }

        /**
         * @brief Set file change callback
         * @param callback Callback to invoke when file changes
         */
        void setFileChangeCallback(FileChangeCallback callback) { file_change_callback_ = callback; }

        /**
         * @brief Set monitoring interval
         * @param interval New check interval
         */
        void setCheckInterval(std::chrono::milliseconds interval) { check_interval_ = interval; }

        /**
         * @brief Get current check interval
         * @return Current check interval
         */
        std::chrono::milliseconds getCheckInterval() const { return check_interval_; }

        /**
         * @brief Get the file path being monitored
         * @return File path
         */
        std::string getFilePath() const { return file_path_; }

        /**
         * @brief Check if file has changed (one-time check)
         * @return true if file has changed since last check, false otherwise
         */
        bool checkFileChanged();

        /**
         * @brief Force a file change check and callback if changed
         * @return true if file changed, false otherwise
         */
        bool forceCheck();

    private:
        std::string file_path_;
        std::chrono::milliseconds check_interval_;
        std::atomic<bool> running_;
        std::thread monitor_thread_;
        FileChangeCallback file_change_callback_;
        std::chrono::system_clock::time_point last_modification_time_;

        /**
         * @brief File monitoring loop (runs in background thread)
         */
        void monitoringLoop();

        /**
         * @brief Check if file has changed since last check
         * @return true if file has changed, false otherwise
         */
        bool hasFileChanged() const;

        /**
         * @brief Notify file change callback
         * @param file_path Path of changed file
         */
        void notifyFileChange(const std::string &file_path);
    };

} // namespace MediaDedup
