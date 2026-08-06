#include "config/config_event_manager.hpp"
#include <iostream>
#include <algorithm>

namespace MediaDedup
{

    size_t ConfigEventManager::subscribeToConfigChanges(ConfigChangeCallback callback)
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        size_t token = next_subscription_token_++;
        config_change_callbacks_.emplace(token, std::move(callback));
        return token;
    }

    void ConfigEventManager::unsubscribeFromConfigChanges(size_t token)
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        config_change_callbacks_.erase(token);
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
        // Copy callbacks while holding the lock to avoid holding mutex during callback execution
        // This prevents deadlock if callbacks attempt blocking operations (e.g., TPM thread pool recreation)
        std::vector<ConfigChangeCallback> callbacks_copy;
        {
            std::lock_guard<std::mutex> lock(callbacks_mutex_);
            callbacks_copy.reserve(config_change_callbacks_.size());
            for (const auto &kv : config_change_callbacks_)
                callbacks_copy.push_back(kv.second);
        }
        
        // Call callbacks outside the lock to prevent deadlock
        for (const auto &callback : callbacks_copy)
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
