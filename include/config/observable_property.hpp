#pragma once

#include <functional>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <any>

namespace MediaDedup
{

    /**
     * @brief Observable property that supports bidirectional updates
     *
     * This class provides a property that can be:
     * - Set programmatically (triggers file update)
     * - Updated from file (triggers callback)
     * - Observed for changes
     * - Validated before updates
     */
    template <typename T>
    class ObservableProperty
    {
    public:
        using ChangeCallback = std::function<void(const T &old_value, const T &new_value)>;
        using ValidationCallback = std::function<bool(const T &value)>;

        /**
         * @brief Constructor
         * @param key Configuration key
         * @param default_value Default value
         * @param description Human-readable description
         */
        ObservableProperty(const std::string &key, const T &default_value, const std::string &description = "")
            : key_(key), value_(default_value), default_value_(default_value), description_(description), is_modified_(false) {}

        /**
         * @brief Destructor
         */
        ~ObservableProperty() = default;

        /**
         * @brief Get the current value
         * @return Current value
         */
        T getValue() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return value_;
        }

        /**
         * @brief Set the value programmatically
         * @param new_value New value to set
         * @param trigger_callback Whether to trigger change callbacks
         * @return true if value was set successfully, false if validation failed
         */
        bool setValue(const T &new_value, bool trigger_callback = true)
        {
            std::lock_guard<std::mutex> lock(mutex_);

            // Validate the new value
            if (validation_callback_ && !validation_callback_(new_value))
            {
                return false;
            }

            T old_value = value_;
            value_ = new_value;
            is_modified_ = true;

            // Trigger change callback if enabled
            if (trigger_callback && change_callback_)
            {
                change_callback_(old_value, new_value);
            }

            return true;
        }

        /**
         * @brief Set the value from file (without triggering callbacks)
         * @param new_value New value from file
         * @return true if value was set successfully, false if validation failed
         */
        bool setValueFromFile(const T &new_value)
        {
            return setValue(new_value, false);
        }

        /**
         * @brief Reset to default value
         */
        void resetToDefault()
        {
            setValue(default_value_);
        }

        /**
         * @brief Get the configuration key
         * @return Configuration key
         */
        const std::string &getKey() const
        {
            return key_;
        }

        /**
         * @brief Get the default value
         * @return Default value
         */
        T getDefaultValue() const
        {
            return default_value_;
        }

        /**
         * @brief Get the description
         * @return Description
         */
        const std::string &getDescription() const
        {
            return description_;
        }

        /**
         * @brief Check if the value has been modified
         * @return true if modified, false otherwise
         */
        bool isModified() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return is_modified_;
        }

        /**
         * @brief Mark as unmodified (after saving to file)
         */
        void markUnmodified()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            is_modified_ = false;
        }

        /**
         * @brief Set change callback
         * @param callback Function to call when value changes
         */
        void setChangeCallback(ChangeCallback callback)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            change_callback_ = std::move(callback);
        }

        /**
         * @brief Set validation callback
         * @param callback Function to validate new values
         */
        void setValidationCallback(ValidationCallback callback)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            validation_callback_ = std::move(callback);
        }

        /**
         * @brief Get the value as a string (for serialization)
         * @return String representation of the value
         */
        std::string getValueAsString() const
        {
            return toString(getValue());
        }

        /**
         * @brief Set the value from a string (for deserialization)
         * @param str String representation of the value
         * @return true if successful, false otherwise
         */
        bool setValueFromString(const std::string &str)
        {
            T value;
            if (fromString(str, value))
            {
                return setValue(value);
            }
            return false;
        }

        /**
         * @brief Implicit conversion to the underlying type
         */
        operator T() const
        {
            return getValue();
        }

    private:
        std::string key_;
        T value_;
        T default_value_;
        std::string description_;
        bool is_modified_;

        ChangeCallback change_callback_;
        ValidationCallback validation_callback_;

        mutable std::mutex mutex_;

        // Helper functions for string conversion
        std::string toString(const T &value) const;
        bool fromString(const std::string &str, T &value) const;
    };

    // Specialization for common types
    template <>
    inline std::string ObservableProperty<std::string>::toString(const std::string &value) const
    {
        return value;
    }

    template <>
    inline bool ObservableProperty<std::string>::fromString(const std::string &str, std::string &value) const
    {
        value = str;
        return true;
    }

    template <>
    inline std::string ObservableProperty<int>::toString(const int &value) const
    {
        return std::to_string(value);
    }

    template <>
    inline bool ObservableProperty<int>::fromString(const std::string &str, int &value) const
    {
        try
        {
            value = std::stoi(str);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    template <>
    inline std::string ObservableProperty<bool>::toString(const bool &value) const
    {
        return value ? "true" : "false";
    }

    template <>
    inline bool ObservableProperty<bool>::fromString(const std::string &str, bool &value) const
    {
        std::string lower = str;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        if (lower == "true" || lower == "1" || lower == "yes" || lower == "on")
        {
            value = true;
            return true;
        }
        else if (lower == "false" || lower == "0" || lower == "no" || lower == "off")
        {
            value = false;
            return true;
        }
        return false;
    }

    template <>
    inline std::string ObservableProperty<double>::toString(const double &value) const
    {
        return std::to_string(value);
    }

    template <>
    inline bool ObservableProperty<double>::fromString(const std::string &str, double &value) const
    {
        try
        {
            value = std::stod(str);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

} // namespace MediaDedup
