#pragma once

#include <string>
#include <any>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <vector>
#include <functional>

// Forward declaration
namespace MediaDedup
{
    class ObservableConfigProperty;
    struct ConfigChangeEvent;

    // Forward declare the callback type
    using ConfigChangeCallback = std::function<void(const ConfigChangeEvent &)>;
}

namespace MediaDedup
{

    /**
     * @brief Manages configuration properties with thread-safe operations
     *
     * Handles storage, retrieval, and management of configuration properties
     * extracted from UnifiedObservableConfigManager for better separation of concerns.
     */
    class ConfigPropertyManager
    {
    public:
        ConfigPropertyManager() = default;
        ~ConfigPropertyManager() = default;

        // Property CRUD operations
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
            properties_[key] = property;
            return property;
        }

        // Property iteration and management
        std::vector<std::string> getAllPropertyKeys() const
        {
            std::lock_guard<std::mutex> lock(properties_mutex_);
            std::vector<std::string> keys;
            keys.reserve(properties_.size());
            for (const auto &[key, _] : properties_)
            {
                keys.push_back(key);
            }
            return keys;
        }

        bool hasProperty(const std::string &key) const
        {
            std::lock_guard<std::mutex> lock(properties_mutex_);
            return properties_.find(key) != properties_.end();
        }

        // Property reset
        void resetToDefaults();

        // Property count
        size_t getPropertyCount() const
        {
            std::lock_guard<std::mutex> lock(properties_mutex_);
            return properties_.size();
        }

        // Clear all properties
        void clear()
        {
            std::lock_guard<std::mutex> lock(properties_mutex_);
            properties_.clear();
        }

        // Set change callback for all properties
        void setGlobalChangeCallback(ConfigChangeCallback callback);

    private:
        std::unordered_map<std::string, std::shared_ptr<ObservableConfigProperty>> properties_;
        mutable std::mutex properties_mutex_;
    };

} // namespace MediaDedup
