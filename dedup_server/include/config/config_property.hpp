#pragma once

#include <string>
#include <any>
#include "config/config_type_converter.hpp"

namespace MediaDedup
{

    /**
     * @brief Base configuration property class
     *
     * Provides core property functionality without observability features.
     * This class handles value storage, type conversion, and basic property management.
     */
    class ConfigProperty
    {
    public:
        /**
         * @brief Constructor
         * @param key Property key/name
         * @param default_value Default value for the property
         * @param description Human-readable description of the property
         */
        ConfigProperty(const std::string &key, const std::any &default_value,
                       const std::string &description = "",
                       const std::string &long_description = "",
                       const std::string &options = "");

        // Getters
        const std::string &getKey() const { return key_; }
        const std::any &getValue() const { return value_; }
        const std::any &getDefaultValue() const { return default_value_; }
        const std::string &getDescription() const { return description_; }
        const std::string &getLongDescription() const { return long_description_; }
        const std::string &getOptions() const { return options_; }
        bool isModified() const { return modified_; }

        // Value type checking and conversion
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
        virtual bool setValueFromString(const std::string &value);

        // Value setting
        virtual bool setValue(const std::any &new_value);
        virtual bool setValueFromFile(const std::any &new_value);

        // Reset to default
        void resetToDefault();

        // Modification tracking
        void markUnmodified() { modified_ = false; }

        // Implicit conversion to stored type (use with caution)
        template <typename T>
        operator T() const { return getValueAs<T>(); }

    protected:
        std::string key_;
        std::any value_;
        std::any default_value_;
        std::string description_;
        std::string long_description_;
        std::string options_;
        bool modified_;

        // Helper method for value setting (without event emission)
        bool setValueInternal(const std::any &new_value);
    };

} // namespace MediaDedup
