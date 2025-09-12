#pragma once

#include <string>
#include <any>
#include <functional>
#include <vector>
#include <mutex>
#include <chrono>

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
         * @brief Subscribe to configuration change events
         * @param callback Callback function to invoke on changes
         */
        void subscribeToConfigChanges(ConfigChangeCallback callback);

        /**
         * @brief Unsubscribe from configuration change events
         * @param callback Callback function to remove (clears all if not found)
         */
        void unsubscribeFromConfigChanges(ConfigChangeCallback callback);

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
        std::vector<ConfigChangeCallback> config_change_callbacks_;
        mutable std::mutex callbacks_mutex_;
        bool filter_file_updates_ = false;

        /**
         * @brief Notify all registered callbacks of a configuration change
         * @param event Event to notify about
         */
        void notifyConfigChange(const ConfigChangeEvent &event);
    };

} // namespace MediaDedup
