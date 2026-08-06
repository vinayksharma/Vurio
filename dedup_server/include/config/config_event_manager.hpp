#pragma once

#include <string>
#include <any>
#include <functional>
#include <vector>
#include <map>
#include <mutex>
#include <chrono>
#include "config/config_change_event.hpp"

namespace MediaDedup
{

    /**
     * @brief Configuration change callback type
     */
    using ConfigChangeCallback = std::function<void(const ConfigChangeEvent &)>;

    /**
     * @brief Manages configuration change events and callbacks
     *
     * Handles event emission, callback registration, and notification
     * with thread-safe operations.
     */
    class ConfigEventManager
    {
    public:
        /**
         * @brief Constructor
         */
        ConfigEventManager() = default;

        /**
         * @brief Destructor
         */
        ~ConfigEventManager() = default;

        // Event subscription management
        /**
         * @brief Subscribe to configuration change events.
         * @return Subscription token for unsubscribeFromConfigChanges.
         *
         * AR-6: token-based. The old callback-parameter unsubscribe could not
         * compare std::functions and CLEARED EVERY SUBSCRIBER in the process
         * (its one caller silently killed config reactivity for the database,
         * TPM, finder, caches, and ONNX manager on media-processor shutdown).
         */
        size_t subscribeToConfigChanges(ConfigChangeCallback callback);

        /**
         * @brief Unsubscribe a single subscription by its token. Unknown
         * tokens are ignored.
         */
        void unsubscribeFromConfigChanges(size_t token);

        /**
         * @brief Clear all event callbacks
         */
        void clearAllCallbacks();

        /**
         * @brief Get number of registered callbacks
         * @return Number of callbacks
         */
        size_t getCallbackCount() const;

        // Event emission
        /**
         * @brief Emit a configuration change event
         * @param event Event to emit
         */
        void emitConfigChangeEvent(const ConfigChangeEvent &event);

        /**
         * @brief Emit a configuration change event with parameters
         * @param key Configuration key that changed
         * @param old_value Previous value
         * @param new_value New value
         * @param source Source of change
         * @param is_file_update Whether this was triggered by file update
         */
        void emitConfigChangeEvent(const std::string &key,
                                   const std::any &old_value,
                                   const std::any &new_value,
                                   const std::string &source = "programmatic",
                                   bool is_file_update = false);

        // Event filtering
        /**
         * @brief Set whether to filter file update events
         * @param filter If true, file update events will not be emitted
         */
        void setFilterFileUpdates(bool filter) { filter_file_updates_ = filter; }

        /**
         * @brief Get whether file update events are filtered
         * @return true if file updates are filtered, false otherwise
         */
        bool isFilteringFileUpdates() const { return filter_file_updates_; }

    private:
        std::map<size_t, ConfigChangeCallback> config_change_callbacks_; // ordered: preserves subscription order
        size_t next_subscription_token_ = 1;
        mutable std::mutex callbacks_mutex_;
        bool filter_file_updates_ = false;

        /**
         * @brief Notify all registered callbacks of a configuration change
         * @param event Event to notify about
         */
        void notifyConfigChange(const ConfigChangeEvent &event);
    };

} // namespace MediaDedup
