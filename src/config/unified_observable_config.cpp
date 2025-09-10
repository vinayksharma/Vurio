#include "config/unified_observable_config.hpp"
#include <algorithm>
#include <iostream>
#include <filesystem>

namespace MediaDedup
{

    // ObservableConfigProperty implementation

    bool ObservableConfigProperty::setValue(const std::any &new_value, const std::string &source)
    {
        // Validate if validation callback is set
        if (validation_callback_ && !validation_callback_(new_value))
        {
            return false;
        }

        // Store old value for event emission
        auto old_value = getValue();

        // Use base class to set the value
        if (!ConfigProperty::setValue(new_value))
        {
            return false;
        }

        // Emit change event
        emitChangeEvent(old_value, new_value, source, false);

        return true;
    }

    bool ObservableConfigProperty::setValueFromFile(const std::any &new_value)
    {
        // Store old value for event emission
        auto old_value = getValue();

        // Use base class to set the value (skip validation for file updates)
        if (!ConfigProperty::setValueFromFile(new_value))
        {
            return false;
        }

        // Emit change event with file source
        emitChangeEvent(old_value, new_value, "file", true);

        return true;
    }

    void ObservableConfigProperty::resetToDefault()
    {
        // Store old value for event emission
        auto old_value = getValue();

        // Use base class to reset to default
        ConfigProperty::resetToDefault();

        // Emit change event
        emitChangeEvent(old_value, getDefaultValue(), "default", false);
    }

    void ObservableConfigProperty::emitChangeEvent(const std::any &old_value, const std::any &new_value,
                                                   const std::string &source, bool is_file_update)
    {
        if (change_callback_)
        {
            ConfigChangeEvent event(key_, old_value, new_value, source, is_file_update);
            change_callback_(event);
        }
    }

    // UnifiedObservableConfigManager implementation

    UnifiedObservableConfigManager::UnifiedObservableConfigManager(const std::string &config_file_path,
                                                                   bool enable_file_monitoring,
                                                                   std::chrono::milliseconds reload_interval)
        : config_file_path_(config_file_path),
          enable_file_monitoring_(enable_file_monitoring),
          reload_interval_(reload_interval),
          running_(false),
          valid_(false)
    {

        last_file_modification_ = std::chrono::system_clock::now();
    }

    UnifiedObservableConfigManager::~UnifiedObservableConfigManager()
    {
        shutdown();
    }

    bool UnifiedObservableConfigManager::initialize()
    {
        try
        {
            // Create config directory if it doesn't exist
            auto config_dir = std::filesystem::path(config_file_path_).parent_path();
            if (!config_dir.empty() && !std::filesystem::exists(config_dir))
            {
                std::filesystem::create_directories(config_dir);
            }

            // Load existing configuration or create default
            if (!loadConfiguration())
            {
                // If loading fails, create default configuration
                valid_ = true;
                return true;
            }

            // If loading succeeds, ensure configuration is marked as valid
            valid_ = true;

            // Start file monitoring if enabled
            if (enable_file_monitoring_)
            {
                startFileMonitoring();
            }

            return true;
        }
        catch (const std::exception &e)
        {
            addValidationError("Initialization failed: " + std::string(e.what()));
            return false;
        }
    }

    void UnifiedObservableConfigManager::shutdown()
    {
        if (running_)
        {
            running_ = false;
            if (file_monitor_thread_.joinable())
            {
                file_monitor_thread_.join();
            }
        }
    }

    bool UnifiedObservableConfigManager::loadConfiguration()
    {
        try
        {
            if (!std::filesystem::exists(config_file_path_))
            {
                // File doesn't exist, create with defaults
                return saveConfiguration();
            }

            return parseYamlFile();
        }
        catch (const std::exception &e)
        {
            addValidationError("Failed to load configuration: " + std::string(e.what()));
            return false;
        }
    }

    bool UnifiedObservableConfigManager::saveConfiguration()
    {
        try
        {
            return serializeToYamlFile();
        }
        catch (const std::exception &e)
        {
            addValidationError("Failed to save configuration: " + std::string(e.what()));
            return false;
        }
    }

    bool UnifiedObservableConfigManager::reloadConfiguration()
    {
        if (loadConfiguration())
        {
            last_file_modification_ = std::chrono::system_clock::now();
            return true;
        }
        return false;
    }

    void UnifiedObservableConfigManager::subscribeToConfigChanges(ConfigChangeCallback callback)
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        config_change_callbacks_.push_back(callback);
    }

    void UnifiedObservableConfigManager::unsubscribeFromConfigChanges(ConfigChangeCallback callback)
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        // Note: std::function doesn't support == operator, so we can't use std::remove
        // For now, we'll just clear all callbacks. In practice, you might want to use
        // a more sophisticated callback management system with unique IDs
        config_change_callbacks_.clear();
    }

    void UnifiedObservableConfigManager::emitConfigChangeEvent(const ConfigChangeEvent &event)
    {
        // Prevent circular references by checking if this is a file update
        if (event.is_file_update)
        {
            // Don't emit events for file updates to avoid infinite loops
            return;
        }

        // Don't save configuration immediately to avoid deadlock
        // Instead, notify subscribers first
        notifyConfigChange(event);

        // Schedule a deferred save operation to avoid deadlock
        // This could be implemented with a background thread or deferred execution
        // For now, we'll skip immediate saving to prevent deadlocks
    }

    bool UnifiedObservableConfigManager::triggerSave()
    {
        // This method can be called manually to save configuration
        // after batch operations or when immediate persistence is needed
        return saveConfiguration();
    }

    void UnifiedObservableConfigManager::setAutoReload(bool enable)
    {
        enable_file_monitoring_ = enable;
        if (enable && !running_)
        {
            startFileMonitoring();
        }
        else if (!enable && running_)
        {
            stopFileMonitoring();
        }
    }

    void UnifiedObservableConfigManager::setReloadInterval(std::chrono::milliseconds interval)
    {
        reload_interval_ = interval;
    }

    // getAllPropertyKeys and hasProperty are now implemented inline in the header
    // and delegate to ConfigPropertyManager

    void UnifiedObservableConfigManager::resetToDefaults()
    {
        property_manager_.resetToDefaults();
        // Don't save immediately to avoid deadlock
        // The change events will handle saving if needed
    }

    std::string UnifiedObservableConfigManager::toString() const
    {
        std::stringstream ss;
        ss << "Configuration Manager Status:\n";
        ss << "  File: " << config_file_path_ << "\n";
        ss << "  Valid: " << (valid_ ? "yes" : "no") << "\n";
        ss << "  Properties: " << property_manager_.getPropertyCount() << "\n";
        ss << "  File monitoring: " << (enable_file_monitoring_ ? "enabled" : "disabled") << "\n";

        if (!validation_errors_.empty())
        {
            ss << "  Validation errors:\n";
            for (const auto &error : validation_errors_)
            {
                ss << "    - " << error << "\n";
            }
        }

        return ss.str();
    }

    // Private methods

    void UnifiedObservableConfigManager::startFileMonitoring()
    {
        if (running_)
            return;

        running_ = true;
        file_monitor_thread_ = std::thread([this]()
                                           { fileMonitoringLoop(); });
    }

    void UnifiedObservableConfigManager::stopFileMonitoring()
    {
        running_ = false;
        if (file_monitor_thread_.joinable())
        {
            file_monitor_thread_.join();
        }
    }

    void UnifiedObservableConfigManager::fileMonitoringLoop()
    {
        while (running_)
        {
            if (hasFileChanged())
            {
                reloadConfiguration();
                notifyFileChange(config_file_path_);
            }
            std::this_thread::sleep_for(reload_interval_);
        }
    }

    bool UnifiedObservableConfigManager::hasFileChanged() const
    {
        try
        {
            if (!std::filesystem::exists(config_file_path_))
            {
                return false;
            }

            auto current_time = std::filesystem::last_write_time(config_file_path_);
            // Convert file time to system time (simplified approach for C++17)
            auto duration = current_time.time_since_epoch();
            auto system_duration = std::chrono::duration_cast<std::chrono::system_clock::duration>(duration);
            auto current_time_sys = std::chrono::system_clock::time_point(system_duration);

            return current_time_sys > last_file_modification_;
        }
        catch (const std::exception &)
        {
            return false;
        }
    }

    bool UnifiedObservableConfigManager::parseYamlFile()
    {
        try
        {
            YAML::Node config = YAML::LoadFile(config_file_path_);

            // Update existing properties from file or create new ones using the underlying type
            for (const auto &item : config)
            {
                std::string key = item.first.Scalar();
                auto value = yamlNodeToAny(item.second);

                auto existing = property_manager_.getProperty<std::any>(key);
                if (existing)
                {
                    existing->setValueFromFile(value);
                    continue;
                }

                if (value.type() == typeid(std::string))
                {
                    createProperty<std::string>(key, std::any_cast<std::string>(value), "Loaded from file");
                }
                else if (value.type() == typeid(int))
                {
                    createProperty<int>(key, std::any_cast<int>(value), "Loaded from file");
                }
                else if (value.type() == typeid(double))
                {
                    createProperty<double>(key, std::any_cast<double>(value), "Loaded from file");
                }
                else if (value.type() == typeid(bool))
                {
                    createProperty<bool>(key, std::any_cast<bool>(value), "Loaded from file");
                }
                else if (value.type() == typeid(std::vector<std::string>))
                {
                    createProperty<std::vector<std::string>>(key, std::any_cast<std::vector<std::string>>(value), "Loaded from file");
                }
                else
                {
                    // Fallback: store as string representation
                    createProperty<std::string>(key, item.second.Scalar(), "Loaded from file");
                }
            }

            valid_ = true;
            clearValidationErrors();
            return true;
        }
        catch (const std::exception &e)
        {
            addValidationError("YAML parsing failed: " + std::string(e.what()));
            valid_ = false;
            return false;
        }
    }

    bool UnifiedObservableConfigManager::serializeToYamlFile() const
    {
        try
        {
            YAML::Node config;

            auto keys = property_manager_.getAllPropertyKeys();
            for (const auto &key : keys)
            {
                auto property = const_cast<ConfigPropertyManager &>(property_manager_).getProperty<std::any>(key);
                if (property)
                {
                    config[key] = anyToYamlNode(property->getValue());
                }
            }

            std::ofstream file(config_file_path_);
            if (!file.is_open())
            {
                return false;
            }

            file << config;
            file.close();

            // Update last modification time (const_cast needed for const method)
            const_cast<UnifiedObservableConfigManager *>(this)->last_file_modification_ = std::chrono::system_clock::now();

            return true;
        }
        catch (const std::exception &e)
        {
            // const_cast needed for const method
            const_cast<UnifiedObservableConfigManager *>(this)->addValidationError("YAML serialization failed: " + std::string(e.what()));
            return false;
        }
    }

    std::any UnifiedObservableConfigManager::yamlNodeToAny(const YAML::Node &node) const
    {
        if (node.IsScalar())
        {
            if (node.IsNull())
            {
                return std::any{};
            }
            else if (node.IsScalar())
            {
                std::string str_value = node.Scalar();

                // Try to convert to appropriate type
                try
                {
                    // Try int first
                    return std::any{node.as<int>()};
                }
                catch (...)
                {
                    try
                    {
                        // Try double
                        return std::any{node.as<double>()};
                    }
                    catch (...)
                    {
                        try
                        {
                            // Try bool
                            return std::any{node.as<bool>()};
                        }
                        catch (...)
                        {
                            // Default to string
                            return std::any{str_value};
                        }
                    }
                }
            }
        }
        else if (node.IsSequence())
        {
            std::vector<std::string> vec;
            for (const auto &item : node)
            {
                vec.push_back(item.Scalar());
            }
            return std::any{vec};
        }
        else if (node.IsMap())
        {
            // For now, convert maps to strings. Could be enhanced to support nested structures
            return std::any{node.Scalar()};
        }

        return std::any{};
    }

    YAML::Node UnifiedObservableConfigManager::anyToYamlNode(const std::any &value) const
    {
        try
        {
            if (value.type() == typeid(std::string))
            {
                return YAML::Node(std::any_cast<std::string>(value));
            }
            else if (value.type() == typeid(int))
            {
                return YAML::Node(std::any_cast<int>(value));
            }
            else if (value.type() == typeid(double))
            {
                return YAML::Node(std::any_cast<double>(value));
            }
            else if (value.type() == typeid(bool))
            {
                return YAML::Node(std::any_cast<bool>(value));
            }
            else if (value.type() == typeid(std::vector<std::string>))
            {
                YAML::Node node;
                auto vec = std::any_cast<std::vector<std::string>>(value);
                for (const auto &item : vec)
                {
                    node.push_back(item);
                }
                return node;
            }
        }
        catch (const std::bad_any_cast &)
        {
            // Fall through to return null
        }

        return YAML::Node();
    }

    void UnifiedObservableConfigManager::notifyFileChange(const std::string &file_path)
    {
        if (file_change_callback_)
        {
            file_change_callback_(file_path);
        }
    }

    ServerMode UnifiedObservableConfigManager::getServerMode(const std::string &key, ServerMode default_mode)
    {
        try
        {
            std::string mode_str = getPropertyValue<std::string>(key, std::string(::MediaDedup::toString(default_mode)));
            return parseServerMode(mode_str);
        }
        catch (...)
        {
            return default_mode;
        }
    }

    void UnifiedObservableConfigManager::notifyConfigChange(const ConfigChangeEvent &event)
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        for (const auto &callback : config_change_callbacks_)
        {
            try
            {
                callback(event);
            }
            catch (const std::exception &e)
            {
                // Log error but don't crash
                std::cerr << "Error in config change callback: " << e.what() << std::endl;
            }
        }
    }

    void UnifiedObservableConfigManager::validateConfiguration()
    {
        clearValidationErrors();

        // Basic validation - check if all required properties exist
        auto keys = property_manager_.getAllPropertyKeys();
        for (const auto &key : keys)
        {
            auto property = property_manager_.getProperty<std::any>(key);
            if (property && property->getValue().type() == typeid(std::string))
            {
                auto str_value = property->getValueAs<std::string>();
                if (str_value.empty())
                {
                    addValidationError("Property '" + key + "' cannot be empty");
                }
            }
        }

        valid_ = validation_errors_.empty();
    }

    void UnifiedObservableConfigManager::addValidationError(const std::string &error)
    {
        validation_errors_.push_back(error);
    }

    void UnifiedObservableConfigManager::clearValidationErrors()
    {
        validation_errors_.clear();
    }

} // namespace MediaDedup
