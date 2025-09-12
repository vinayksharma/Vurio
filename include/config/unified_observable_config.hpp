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
#include "config/property_manager.hpp"
#include "config/config_property.hpp"
#include "config/yaml_serializer.hpp"

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
     * Extends ConfigProperty with event emission and validation capabilities
     */
    class ObservableConfigProperty : public ConfigProperty
    {
    public:
        using ChangeCallback = std::function<void(const ConfigChangeEvent &)>;
        using ValidationCallback = std::function<bool(const std::any &)>;

        ObservableConfigProperty(const std::string &key, const std::any &default_value,
                                 const std::string &description = "")
            : ConfigProperty(key, default_value, description) {}

        // Value setting with validation and event emission
        bool setValue(const std::any &new_value, const std::string &source = "programmatic");
        bool setValueFromFile(const std::any &new_value);

        // Reset to default (override to emit events)
        void resetToDefault();

        // Callback management
        void setChangeCallback(ChangeCallback callback) { change_callback_ = callback; }
        void setValidationCallback(ValidationCallback callback) { validation_callback_ = callback; }

    private:
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

        // Property management - delegated to ConfigPropertyManager
        template <typename T>
        std::shared_ptr<ObservableConfigProperty> getProperty(const std::string &key)
        {
            return property_manager_.getProperty<T>(key);
        }

        template <typename T>
        bool setPropertyValue(const std::string &key, const T &value)
        {
            return property_manager_.setPropertyValue<T>(key, value);
        }

        template <typename T>
        T getPropertyValue(const std::string &key, const T &default_value = T{})
        {
            return property_manager_.getPropertyValue<T>(key, default_value);
        }

        // Property creation and registration
        template <typename T>
        std::shared_ptr<ObservableConfigProperty> createProperty(const std::string &key,
                                                                 const T &default_value,
                                                                 const std::string &description = "")
        {
            auto property = property_manager_.createProperty<T>(key, default_value, description);

            // Set up change callback to emit events
            property->setChangeCallback([this](const ConfigChangeEvent &event)
                                        { emitConfigChangeEvent(event); });

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
        std::vector<std::string> getAllPropertyKeys() const { return property_manager_.getAllPropertyKeys(); }
        bool hasProperty(const std::string &key) const { return property_manager_.hasProperty(key); }

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

        ConfigPropertyManager property_manager_;
        YamlConfigSerializer yaml_serializer_;

        std::vector<ConfigChangeCallback> config_change_callbacks_;
        mutable std::mutex callbacks_mutex_;

        FileChangeCallback file_change_callback_;

        std::atomic<bool> running_;
        std::thread file_monitor_thread_;
        std::filesystem::file_time_type last_file_modification_;

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

        // Event emission helpers
        void notifyFileChange(const std::string &file_path);
        void notifyConfigChange(const ConfigChangeEvent &event);

        // Validation
        void validateConfiguration();
        void addValidationError(const std::string &error);
        void clearValidationErrors();
    };

} // namespace MediaDedup
