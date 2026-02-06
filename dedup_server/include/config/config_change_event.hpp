#pragma once

#include <string>
#include <any>
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

        /**
         * @brief Constructor
         * @param k Configuration key that changed
         * @param old_val Previous value
         * @param new_val New value
         * @param src Source of change (default: "programmatic")
         * @param file_update Whether this was triggered by file update (default: false)
         */
        ConfigChangeEvent(const std::string &k, const std::any &old_val, const std::any &new_val,
                          const std::string &src = "programmatic", bool file_update = false);

        /**
         * @brief Get string representation of the event
         * @return Formatted string describing the event
         */
        std::string toString() const;

        /**
         * @brief Check if the event represents an actual value change
         * @return true if values are different, false if they are the same
         */
        bool hasValueChanged() const;

        /**
         * @brief Get the time since the event occurred
         * @return Duration since the event timestamp
         */
        std::chrono::milliseconds getAge() const;
    };

} // namespace MediaDedup
