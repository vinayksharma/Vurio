#include "config/config_validator.hpp"
#include <sstream>
#include <regex>

namespace MediaDedup
{

    ConfigValidator::ConfigValidator() : validation_enabled_(true)
    {
    }

    bool ConfigValidator::validateConfiguration(const std::unordered_map<std::string, std::any> &properties)
    {
        clearValidationErrors();

        if (!validation_enabled_)
        {
            return true;
        }

        bool is_valid = true;

        // Validate server configuration
        if (!validateServerConfig(properties))
        {
            is_valid = false;
        }

        // Validate logging configuration
        if (!validateLoggingConfig(properties))
        {
            is_valid = false;
        }

        // Validate database configuration
        if (!validateDatabaseConfig(properties))
        {
            is_valid = false;
        }

        // Validate file scanner configuration
        if (!validateFileScannerConfig(properties))
        {
            is_valid = false;
        }

        // Validate individual properties with custom callbacks
        for (const auto &pair : properties)
        {
            if (!validateProperty(pair.first, pair.second))
            {
                is_valid = false;
            }
        }

        return is_valid;
    }

    bool ConfigValidator::validateProperty(const std::string &key, const std::any &value)
    {
        if (!validation_enabled_)
        {
            return true;
        }

        // Check for custom validation callback
        auto it = validation_callbacks_.find(key);
        if (it != validation_callbacks_.end())
        {
            try
            {
                if (!it->second(key, value))
                {
                    addValidationError(key, "Custom validation failed", "", "");
                    return false;
                }
            }
            catch (const std::exception &e)
            {
                addValidationError(key, "Validation callback error: " + std::string(e.what()), "", "");
                return false;
            }
        }

        // Perform built-in validation
        return performBuiltInValidation(key, value);
    }

    void ConfigValidator::addValidationError(const std::string &key, const std::string &message,
                                             const std::string &expected, const std::string &actual)
    {
        validation_errors_.emplace_back(key, message, expected, actual);
    }

    void ConfigValidator::clearValidationErrors()
    {
        validation_errors_.clear();
    }

    const std::vector<ValidationError> &ConfigValidator::getValidationErrors() const
    {
        return validation_errors_;
    }

    bool ConfigValidator::isValid() const
    {
        return validation_errors_.empty();
    }

    std::string ConfigValidator::getValidationStatus() const
    {
        if (validation_errors_.empty())
        {
            return "Configuration is valid";
        }

        std::ostringstream oss;
        oss << "Configuration has " << validation_errors_.size() << " validation error(s):\n";

        for (const auto &error : validation_errors_)
        {
            oss << "  - " << error.key << ": " << error.message;
            if (!error.expected.empty())
            {
                oss << " (expected: " << error.expected;
                if (!error.actual.empty())
                {
                    oss << ", actual: " << error.actual;
                }
                oss << ")";
            }
            oss << "\n";
        }

        return oss.str();
    }

    void ConfigValidator::registerValidationCallback(const std::string &key, ValidationCallback callback)
    {
        validation_callbacks_[key] = callback;
    }

    void ConfigValidator::unregisterValidationCallback(const std::string &key)
    {
        validation_callbacks_.erase(key);
    }

    void ConfigValidator::setValidationEnabled(bool enabled)
    {
        validation_enabled_ = enabled;
    }

    bool ConfigValidator::isValidationEnabled() const
    {
        return validation_enabled_;
    }

    bool ConfigValidator::performBuiltInValidation(const std::string &key, const std::any &value)
    {
        // Validate server.port
        if (key == "server.port")
        {
            try
            {
                int port = std::any_cast<int>(value);
                if (port < 1 || port > 65535)
                {
                    addValidationError(key, "Port must be between 1 and 65535",
                                       "1-65535", std::to_string(port));
                    return false;
                }
            }
            catch (const std::bad_any_cast &)
            {
                addValidationError(key, "Port must be an integer", "integer", "non-integer");
                return false;
            }
        }

        // Validate server.host
        if (key == "server.host")
        {
            try
            {
                std::string host = std::any_cast<std::string>(value);
                if (host.empty())
                {
                    addValidationError(key, "Host cannot be empty", "non-empty string", "empty");
                    return false;
                }
            }
            catch (const std::bad_any_cast &)
            {
                addValidationError(key, "Host must be a string", "string", "non-string");
                return false;
            }
        }

        // Validate logging.level
        if (key == "logging.level")
        {
            try
            {
                std::string level = std::any_cast<std::string>(value);
                std::vector<std::string> valid_levels = {"trace", "debug", "info", "warn", "error"};
                if (std::find(valid_levels.begin(), valid_levels.end(), level) == valid_levels.end())
                {
                    addValidationError(key, "Invalid log level",
                                       "trace|debug|info|warn|error", level);
                    return false;
                }
            }
            catch (const std::bad_any_cast &)
            {
                addValidationError(key, "Log level must be a string", "string", "non-string");
                return false;
            }
        }

        // Validate debug.enabled
        if (key == "debug.enabled")
        {
            try
            {
                std::any_cast<bool>(value);
            }
            catch (const std::bad_any_cast &)
            {
                addValidationError(key, "Debug enabled must be a boolean", "boolean", "non-boolean");
                return false;
            }
        }

        // Validate server.max_connections
        if (key == "server.max_connections")
        {
            try
            {
                int max_conn = std::any_cast<int>(value);
                if (max_conn < 1 || max_conn > 10000)
                {
                    addValidationError(key, "Max connections must be between 1 and 10000",
                                       "1-10000", std::to_string(max_conn));
                    return false;
                }
            }
            catch (const std::bad_any_cast &)
            {
                addValidationError(key, "Max connections must be an integer", "integer", "non-integer");
                return false;
            }
        }

        // Validate server.timeout
        if (key == "server.timeout")
        {
            try
            {
                double timeout = std::any_cast<double>(value);
                if (timeout < 0.1 || timeout > 3600.0)
                {
                    addValidationError(key, "Timeout must be between 0.1 and 3600 seconds",
                                       "0.1-3600", std::to_string(timeout));
                    return false;
                }
            }
            catch (const std::bad_any_cast &)
            {
                addValidationError(key, "Timeout must be a number", "number", "non-number");
                return false;
            }
        }

        return true;
    }

    bool ConfigValidator::validateServerConfig(const std::unordered_map<std::string, std::any> &properties)
    {
        bool is_valid = true;

        // Validate server properties if they exist
        auto host_it = properties.find("server.host");
        if (host_it != properties.end())
        {
            if (!validateProperty("server.host", host_it->second))
            {
                is_valid = false;
            }
        }

        auto port_it = properties.find("server.port");
        if (port_it != properties.end())
        {
            if (!validateProperty("server.port", port_it->second))
            {
                is_valid = false;
            }
        }

        return is_valid;
    }

    bool ConfigValidator::validateLoggingConfig(const std::unordered_map<std::string, std::any> &properties)
    {
        bool is_valid = true;

        // Check logging.level if present
        auto level_it = properties.find("logging.level");
        if (level_it != properties.end())
        {
            if (!validateProperty("logging.level", level_it->second))
            {
                is_valid = false;
            }
        }

        return is_valid;
    }

    bool ConfigValidator::validateDatabaseConfig(const std::unordered_map<std::string, std::any> &properties)
    {
        // Database validation can be added here as needed
        return true;
    }

    bool ConfigValidator::validateFileScannerConfig(const std::unordered_map<std::string, std::any> &properties)
    {
        // File scanner validation can be added here as needed
        return true;
    }

} // namespace MediaDedup
