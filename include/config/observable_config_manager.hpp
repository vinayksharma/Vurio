#pragma once

#include "observable_property.hpp"
#include "observable_log_level.hpp"
#include "log_level.hpp"
#include <Poco/Util/PropertyFileConfiguration.h>
#include <Poco/Util/IniFileConfiguration.h>
#include <Poco/File.h>
#include <Poco/Path.h>
#include <Poco/Logger.h>
#include <memory>
#include <unordered_map>
#include <functional>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <atomic>

namespace MediaDedup
{

    /**
     * @brief Observable configuration manager with bidirectional updates
     *
     * This class provides:
     * - Observable properties that can be watched for changes
     * - Automatic file updates when properties change programmatically
     * - File change detection and property updates
     * - Validation and change callbacks
     * - Thread-safe operations
     */
    class ObservableConfigManager
    {
    public:
        using FileChangeCallback = std::function<void(const std::string &file_path)>;
        using PropertyChangeCallback = std::function<void(const std::string &key, const std::string &old_value, const std::string &new_value)>;

        /**
         * @brief Constructor
         * @param config_file_path Path to configuration file
         * @param auto_reload Enable automatic file reloading
         * @param reload_interval Reload interval in milliseconds
         */
        explicit ObservableConfigManager(const std::string &config_file_path = "config/logging.yaml",
                                         bool auto_reload = true,
                                         std::chrono::milliseconds reload_interval = std::chrono::milliseconds(1000));

        /**
         * @brief Destructor
         */
        ~ObservableConfigManager();

        /**
         * @brief Initialize the configuration manager
         * @return true if successful, false otherwise
         */
        bool initialize();

        /**
         * @brief Load configuration from file
         * @param config_file_path Path to configuration file
         * @return true if successful, false otherwise
         */
        bool loadConfiguration(const std::string &config_file_path = "");

        /**
         * @brief Save configuration to file
         * @return true if successful, false otherwise
         */
        bool saveConfiguration();

        /**
         * @brief Reload configuration from file
         * @return true if successful, false otherwise
         */
        bool reloadConfiguration();

        /**
         * @brief Get observable property by key
         * @param key Configuration key
         * @return Pointer to observable property, or nullptr if not found
         */
        template <typename T>
        std::shared_ptr<ObservableProperty<T>> getProperty(const std::string &key) const;

        /**
         * @brief Get observable log level property
         * @param key Configuration key
         * @return Pointer to observable log level property, or nullptr if not found
         */
        std::shared_ptr<ObservableLogLevel> getLogLevelProperty(const std::string &key) const;

        /**
         * @brief Set property value programmatically
         * @param key Configuration key
         * @param value New value
         * @return true if successful, false otherwise
         */
        template <typename T>
        bool setPropertyValue(const std::string &key, const T &value);

        /**
         * @brief Get property value
         * @param key Configuration key
         * @param default_value Default value if property not found
         * @return Property value
         */
        template <typename T>
        T getPropertyValue(const std::string &key, const T &default_value) const;

        /**
         * @brief Check if property exists
         * @param key Configuration key
         * @return true if property exists, false otherwise
         */
        bool hasProperty(const std::string &key) const;

        /**
         * @brief Get all property keys
         * @return Vector of all property keys
         */
        std::vector<std::string> getAllPropertyKeys() const;

        /**
         * @brief Set file change callback
         * @param callback Function to call when file changes
         */
        void setFileChangeCallback(FileChangeCallback callback);

        /**
         * @brief Set property change callback
         * @param callback Function to call when any property changes
         */
        void setPropertyChangeCallback(PropertyChangeCallback callback);

        /**
         * @brief Enable/disable automatic file reloading
         * @param enable Whether to enable auto-reload
         */
        void setAutoReload(bool enable);

        /**
         * @brief Set reload interval
         * @param interval Reload interval in milliseconds
         */
        void setReloadInterval(std::chrono::milliseconds interval);

        /**
         * @brief Get configuration file path
         * @return Configuration file path
         */
        std::string getConfigFilePath() const;

        /**
         * @brief Check if configuration is valid
         * @return true if valid, false otherwise
         */
        bool isValid() const;

        /**
         * @brief Get validation errors
         * @return Vector of validation error messages
         */
        std::vector<std::string> getValidationErrors() const;

        /**
         * @brief Reset all properties to default values
         */
        void resetToDefaults();

        /**
         * @brief Get configuration as string (for debugging)
         * @return String representation of current configuration
         */
        std::string toString() const;

    private:
        // Configuration file management
        std::string config_file_path_;
        std::unique_ptr<Poco::Util::PropertyFileConfiguration> config_;
        Poco::Logger &logger_;

        // Observable properties storage
        std::unordered_map<std::string, std::shared_ptr<void>> properties_;
        mutable std::mutex properties_mutex_;

        // File monitoring
        std::atomic<bool> auto_reload_;
        std::chrono::milliseconds reload_interval_;
        std::atomic<bool> stop_monitoring_;
        std::thread file_monitor_thread_;

        // File change detection
        std::chrono::system_clock::time_point last_file_modification_;
        std::string last_file_content_hash_;

        // Callbacks
        FileChangeCallback file_change_callback_;
        PropertyChangeCallback property_change_callback_;

        // Validation
        std::vector<std::string> validation_errors_;
        bool is_valid_;

        /**
         * @brief Start file monitoring thread
         */
        void startFileMonitoring();

        /**
         * @brief Stop file monitoring thread
         */
        void stopFileMonitoring();

        /**
         * @brief File monitoring loop
         */
        void fileMonitoringLoop();

        /**
         * @brief Check if file has changed
         * @return true if file has changed, false otherwise
         */
        bool hasFileChanged();

        /**
         * @brief Calculate file content hash
         * @return Hash of file content
         */
        std::string calculateFileHash();

        /**
         * @brief Update properties from file
         */
        void updatePropertiesFromFile();

        /**
         * @brief Create observable property from configuration
         * @param key Configuration key
         * @param value Configuration value
         */
        void createObservableProperty(const std::string &key, const std::string &value);

        /**
         * @brief Validate configuration
         */
        void validateConfiguration();

        /**
         * @brief Log property change
         * @param key Property key
         * @param old_value Old value
         * @param new_value New value
         */
        void logPropertyChange(const std::string &key, const std::string &old_value, const std::string &new_value);
    };

    // Template method implementations
    template <typename T>
    std::shared_ptr<ObservableProperty<T>> ObservableConfigManager::getProperty(const std::string &key) const
    {
        std::lock_guard<std::mutex> lock(properties_mutex_);

        auto it = properties_.find(key);
        if (it != properties_.end())
        {
            try
            {
                return std::static_pointer_cast<ObservableProperty<T>>(it->second);
            }
            catch (const std::bad_cast &)
            {
                logger_.warning("Property " + key + " is not of the expected type");
                return nullptr;
            }
        }

        return nullptr;
    }

    template <typename T>
    bool ObservableConfigManager::setPropertyValue(const std::string &key, const T &value)
    {
        auto property = getProperty<T>(key);
        if (property)
        {
            bool success = property->setValue(value);
            if (success)
            {
                // Trigger file save
                saveConfiguration();
            }
            return success;
        }
        return false;
    }

    template <typename T>
    T ObservableConfigManager::getPropertyValue(const std::string &key, const T &default_value) const
    {
        auto property = getProperty<T>(key);
        if (property)
        {
            return property->getValue();
        }
        return default_value;
    }

} // namespace MediaDedup
