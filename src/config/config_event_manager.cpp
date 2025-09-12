#include "config/config_event_manager.hpp"
#include <iostream>
#include <algorithm>

namespace MediaDedup
{

    void ConfigEventManager::subscribeToConfigChanges(ConfigChangeCallback callback)
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        config_change_callbacks_.push_back(callback);
    }

    void ConfigEventManager::unsubscribeFromConfigChanges(ConfigChangeCallback callback)
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        // Note: std::function doesn't support == operator, so we can't use std::remove
        // For now, we'll just clear all callbacks. In practice, you might want to use
        // a more sophisticated callback management system with unique IDs
        config_change_callbacks_.clear();
    }

    void ConfigEventManager::clearAllCallbacks()
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        config_change_callbacks_.clear();
    }

    size_t ConfigEventManager::getCallbackCount() const
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        return config_change_callbacks_.size();
    }

    void ConfigEventManager::emitConfigChangeEvent(const ConfigChangeEvent &event)
    {
        // Filter file update events if enabled
        if (filter_file_updates_ && event.is_file_update)
        {
            return;
        }

        notifyConfigChange(event);
    }

    void ConfigEventManager::emitConfigChangeEvent(const std::string &key,
                                                   const std::any &old_value,
                                                   const std::any &new_value,
                                                   const std::string &source,
                                                   bool is_file_update)
    {
        ConfigChangeEvent event(key, old_value, new_value, source, is_file_update);
        emitConfigChangeEvent(event);
    }

    void ConfigEventManager::notifyConfigChange(const ConfigChangeEvent &event)
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

} // namespace MediaDedup
