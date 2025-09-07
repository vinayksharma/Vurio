#pragma once

#include <string>
#include <any>
#include <functional>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <yaml-cpp/yaml.h>
#include "config/config_enums.hpp"

namespace MediaDedup
{

    /**
     * @brief Configuration change event structure
     *
     * Represents a configuration change with key, old value, new value, and metadata
     */
    struct ConfigChangeEvent
    {
        std::string key;                                 // Configuration key that changed
        std::any old_value;                              // Previous value
        std::any new_value;                              // New value
        std::string source;                              // Source of change ("file", "programmatic", "default")
        std::chrono::system_clock::time_point timestamp; // When the change occurred
        bool is_file_update;                             // Whether this was triggered by file update

        ConfigChangeEvent(const std::string &k, const std::any &old_val, const std::any &new_val,
                          const std::string &src = "programmatic", bool file_update = false)
            : key(k), old_value(old_val), new_value(new_val), source(src),
              timestamp(std::chrono::system_clock::now()), is_file_update(file_update) {}
    };

    /**
     * @brief Configuration property wrapper with unified observability
     *
     * Generic property wrapper that can hold any type and emit change events
     */
    class ObservableConfigProperty
    {
    public:
        using ChangeCallback = std::function<void(const ConfigChangeEvent &)>;
        using ValidationCallback = std::function<bool(const std::any &)>;

        ObservableConfigProperty(const std::string &key, const std::any &default_value,
                                 const std::string &description = "")
            : key_(key), value_(default_value), default_value_(default_value),
              description_(description), modified_(false) {}

        // Getters
        const std::string &getKey() const { return key_; }
        const std::any &getValue() const { return value_; }
        const std::any &getDefaultValue() const { return default_value_; }
        const std::string &getDescription() const { return description_; }
        bool isModified() const { return modified_; }

        // Value type checking
        template <typename T>
        T getValueAs() const
        {
            try
            {
                return std::any_cast<T>(value_);
            }
            catch (const std::bad_any_cast &)
            {
                return T{};
            }
        }

        // String conversion for file storage
        std::string getValueAsString() const;
        bool setValueFromString(const std::string &value);

        // Value setting with validation and event emission
        bool setValue(const std::any &new_value, const std::string &source = "programmatic");
        bool setValueFromFile(const std::any &new_value);

        // Reset to default
        void resetToDefault();

        // Callback management
        void setChangeCallback(ChangeCallback callback) { change_callback_ = callback; }
        void setValidationCallback(ValidationCallback callback) { validation_callback_ = callback; }

        // Modification tracking
        void markUnmodified() { modified_ = false; }

        // Implicit conversion to stored type (use with caution)
        template <typename T>
        operator T() const { return getValueAs<T>(); }

    private:
        std::string key_;
        std::any value_;
        std::any default_value_;
        std::string description_;
        bool modified_;

        ChangeCallback change_callback_;
        ValidationCallback validation_callback_;

        void emitChangeEvent(const std::any &old_value, const std::any &new_value,
                             const std::string &source, bool is_file_update);
    };

    /**
     * @brief Unified configuration manager with event-driven observability
     *
     * Manages all configuration properties through a unified event system
     */
    class UnifiedObservableConfigManager
    {
    public:
        using ConfigChangeCallback = std::function<void(const ConfigChangeEvent &)>;
        using FileChangeCallback = std::function<void(const std::string &)>;

        UnifiedObservableConfigManager(const std::string &config_file_path,
                                       bool enable_file_monitoring = true,
                                       std::chrono::milliseconds reload_interval = std::chrono::milliseconds(1000));

        ~UnifiedObservableConfigManager();

        // Initialization and lifecycle
        bool initialize();
        void shutdown();
        bool loadConfiguration();
        bool saveConfiguration();
        bool reloadConfiguration();

        // Property management
        template <typename T>
        std::shared_ptr<ObservableConfigProperty> getProperty(const std::string &key)
        {
            std::lock_guard<std::mutex> lock(properties_mutex_);
            auto it = properties_.find(key);
            if (it != properties_.end())
            {
                return it->second;
            }
            return nullptr;
        }

        template <typename T>
        bool setPropertyValue(const std::string &key, const T &value)
        {
            auto property = getProperty<T>(key);
            if (property)
            {
                return property->setValue(value);
            }
            return false;
        }

        template <typename T>
        T getPropertyValue(const std::string &key, const T &default_value = T{})
        {
            auto property = getProperty<T>(key);
            if (property)
            {
                return property->template getValueAs<T>();
            }
            return default_value;
        }

        // Property creation and registration
        template <typename T>
        std::shared_ptr<ObservableConfigProperty> createProperty(const std::string &key,
                                                                 const T &default_value,
                                                                 const std::string &description = "")
        {
            std::lock_guard<std::mutex> lock(properties_mutex_);

            auto property = std::make_shared<ObservableConfigProperty>(key, default_value, description);

            // Set up change callback to emit events
            property->setChangeCallback([this](const ConfigChangeEvent &event)
                                        { emitConfigChangeEvent(event); });

            properties_[key] = property;
            return property;
        }

        // Event system
        void subscribeToConfigChanges(ConfigChangeCallback callback);
        void unsubscribeFromConfigChanges(ConfigChangeCallback callback);
        void emitConfigChangeEvent(const ConfigChangeEvent &event);

        // Manual save trigger (to be called after batch operations)
        bool triggerSave();

        // File monitoring
        void setFileChangeCallback(FileChangeCallback callback) { file_change_callback_ = callback; }
        void setAutoReload(bool enable);
        void setReloadInterval(std::chrono::milliseconds interval);

        // Configuration validation and status
        bool isValid() const { return valid_; }
        std::vector<std::string> getValidationErrors() const { return validation_errors_; }
        std::vector<std::string> getAllPropertyKeys() const;
        bool hasProperty(const std::string &key) const;

        // Utility methods
        std::string getConfigFilePath() const { return config_file_path_; }
        void resetToDefaults();
        std::string toString() const;

        // Convenience: get server processing mode as enum
        ServerMode getServerMode(const std::string &key = "server.mode",
                                 ServerMode default_mode = ServerMode::FAST);

    private:
        std::string config_file_path_;
        bool enable_file_monitoring_;
        std::chrono::milliseconds reload_interval_;

        std::unordered_map<std::string, std::shared_ptr<ObservableConfigProperty>> properties_;
        mutable std::mutex properties_mutex_;

        std::vector<ConfigChangeCallback> config_change_callbacks_;
        mutable std::mutex callbacks_mutex_;

        FileChangeCallback file_change_callback_;

        std::atomic<bool> running_;
        std::thread file_monitor_thread_;
        std::chrono::system_clock::time_point last_file_modification_;

        std::atomic<bool> valid_;
        std::vector<std::string> validation_errors_;

        // File monitoring
        void startFileMonitoring();
        void stopFileMonitoring();
        void fileMonitoringLoop();
        bool hasFileChanged() const;

        // YAML parsing and serialization
        bool parseYamlFile();
        bool serializeToYamlFile() const;
        std::any yamlNodeToAny(const YAML::Node &node) const;
        YAML::Node anyToYamlNode(const std::any &value) const;

        // Event emission helpers
        void notifyFileChange(const std::string &file_path);
        void notifyConfigChange(const ConfigChangeEvent &event);

        // Validation
        void validateConfiguration();
        void addValidationError(const std::string &error);
        void clearValidationErrors();
    };

} // namespace MediaDedup
