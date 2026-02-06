#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <any>
#include <functional>

namespace MediaDedup
{

    /**
     * @brief Configuration validation error structure
     */
    struct ValidationError
    {
        std::string key;      // Configuration key that failed validation
        std::string message;  // Error message describing the validation failure
        std::string expected; // Expected value or format
        std::string actual;   // Actual value that caused the failure

        ValidationError(const std::string &k, const std::string &msg,
                        const std::string &exp = "", const std::string &act = "")
            : key(k), message(msg), expected(exp), actual(act) {}
    };

    /**
     * @brief Configuration validation callback type
     */
    using ValidationCallback = std::function<bool(const std::string &, const std::any &)>;

    /**
     * @brief Manages configuration validation logic and error tracking.
     */
    class ConfigValidator
    {
    public:
        ConfigValidator();
        ~ConfigValidator() = default;

        /**
         * @brief Validates the entire configuration
         * @param properties Configuration properties to validate
         * @return true if configuration is valid, false otherwise
         */
        bool validateConfiguration(const std::unordered_map<std::string, std::any> &properties);

        /**
         * @brief Validates a single property
         * @param key Property key
         * @param value Property value
         * @return true if property is valid, false otherwise
         */
        bool validateProperty(const std::string &key, const std::any &value);

        /**
         * @brief Add a validation error
         * @param key Configuration key that failed validation
         * @param message Error message
         * @param expected Expected value or format
         * @param actual Actual value that caused the failure
         */
        void addValidationError(const std::string &key, const std::string &message,
                                const std::string &expected = "", const std::string &actual = "");

        /**
         * @brief Clear all validation errors
         */
        void clearValidationErrors();

        /**
         * @brief Get all validation errors
         * @return Vector of validation errors
         */
        const std::vector<ValidationError> &getValidationErrors() const;

        /**
         * @brief Check if configuration is valid
         * @return true if valid, false if there are validation errors
         */
        bool isValid() const;

        /**
         * @brief Get validation status as string
         * @return Formatted string describing validation status
         */
        std::string getValidationStatus() const;

        /**
         * @brief Register a custom validation callback for a property
         * @param key Property key to validate
         * @param callback Validation function
         */
        void registerValidationCallback(const std::string &key, ValidationCallback callback);

        /**
         * @brief Unregister a validation callback
         * @param key Property key
         */
        void unregisterValidationCallback(const std::string &key);

        /**
         * @brief Set validation enabled/disabled
         * @param enabled Whether validation should be performed
         */
        void setValidationEnabled(bool enabled);

        /**
         * @brief Check if validation is enabled
         * @return true if validation is enabled
         */
        bool isValidationEnabled() const;

    private:
        std::vector<ValidationError> validation_errors_;
        std::unordered_map<std::string, ValidationCallback> validation_callbacks_;
        bool validation_enabled_;

        /**
         * @brief Perform built-in validation for common property types
         * @param key Property key
         * @param value Property value
         * @return true if valid, false otherwise
         */
        bool performBuiltInValidation(const std::string &key, const std::any &value);

        /**
         * @brief Validate server configuration properties
         * @param properties Configuration properties
         * @return true if valid, false otherwise
         */
        bool validateServerConfig(const std::unordered_map<std::string, std::any> &properties);

        /**
         * @brief Validate logging configuration properties
         * @param properties Configuration properties
         * @return true if valid, false otherwise
         */
        bool validateLoggingConfig(const std::unordered_map<std::string, std::any> &properties);

        /**
         * @brief Validate database configuration properties
         * @param properties Configuration properties
         * @return true if valid, false otherwise
         */
        bool validateDatabaseConfig(const std::unordered_map<std::string, std::any> &properties);

        /**
         * @brief Validate file scanner configuration properties
         * @param properties Configuration properties
         * @return true if valid, false otherwise
         */
        bool validateFileScannerConfig(const std::unordered_map<std::string, std::any> &properties);
    };

} // namespace MediaDedup
