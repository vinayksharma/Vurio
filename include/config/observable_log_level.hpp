#pragma once

#include "observable_property.hpp"
#include "log_level.hpp"
#include <algorithm>

namespace MediaDedup
{

    /**
     * @brief Specialized observable property for LogLevel
     *
     * This class extends ObservableProperty<LogLevel> with:
     * - Log level validation
     * - String conversion for YAML/JSON
     * - Default change callbacks for logging system updates
     */
    class ObservableLogLevel : public ObservableProperty<LogLevel>
    {
    public:
        /**
         * @brief Constructor
         * @param key Configuration key
         * @param default_level Default log level
         * @param description Human-readable description
         */
        ObservableLogLevel(const std::string &key, LogLevel default_level, const std::string &description = "")
            : ObservableProperty<LogLevel>(key, default_level, description)
        {

            // Set up validation callback
            setValidationCallback([](const LogLevel &level) -> bool
                                  {
            // All log levels are valid
            return true; });

            // Set up default change callback for logging system updates
            setChangeCallback([](const LogLevel &old_level, const LogLevel &new_level)
                              {
                                  // This callback will be called when the log level changes
                                  // It can be used to update the logging system in real-time
                              });
        }

        /**
         * @brief Get the value as a string (for serialization)
         * @return String representation of the log level
         */
        std::string getValueAsString() const
        {
            return logLevelToString(getValue());
        }

        /**
         * @brief Set the value from a string (for deserialization)
         * @param str String representation of the log level
         * @return true if successful, false otherwise
         */
        bool setValueFromString(const std::string &str)
        {
            LogLevel level = stringToLogLevel(str);
            return setValue(level);
        }

        /**
         * @brief Check if a given log level is enabled
         * @param level Log level to check
         * @return true if level is enabled, false otherwise
         */
        bool isLevelEnabled(LogLevel level) const
        {
            return isLogLevelEnabled(level, getValue());
        }

        /**
         * @brief Get all available log level options
         * @return Vector of all available log level strings
         */
        static std::vector<std::string> getAvailableOptions()
        {
            return getAllLogLevels();
        }

        /**
         * @brief Get description for a specific log level
         * @param level Log level
         * @return Description of the log level
         */
        static std::string getLevelDescription(LogLevel level)
        {
            return getLogLevelDescription(level);
        }

        /**
         * @brief Get current level description
         * @return Description of the current log level
         */
        std::string getCurrentLevelDescription() const
        {
            return getLevelDescription(getValue());
        }

        /**
         * @brief Set log level with string validation
         * @param level_str String representation of log level
         * @return true if successful, false if invalid string
         */
        bool setLogLevelFromString(const std::string &level_str)
        {
            std::string lower = level_str;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

            // Check if the string is a valid log level
            auto valid_levels = getAllLogLevels();
            if (std::find(valid_levels.begin(), valid_levels.end(), lower) == valid_levels.end())
            {
                return false;
            }

            LogLevel level = stringToLogLevel(lower);
            return setValue(level);
        }

        /**
         * @brief Increase log level (make more verbose)
         * @return true if successful, false if already at most verbose
         */
        bool increaseLevel()
        {
            LogLevel current = getValue();
            if (static_cast<int>(current) > 0)
            {
                return setValue(static_cast<LogLevel>(static_cast<int>(current) - 1));
            }
            return false;
        }

        /**
         * @brief Decrease log level (make less verbose)
         * @return true if successful, false if already at least verbose
         */
        bool decreaseLevel()
        {
            LogLevel current = getValue();
            if (static_cast<int>(current) < 5)
            {
                return setValue(static_cast<LogLevel>(static_cast<int>(current) + 1));
            }
            return false;
        }

        /**
         * @brief Set to most verbose level (TRACE)
         */
        void setMostVerbose()
        {
            setValue(LogLevel::TRACE);
        }

        /**
         * @brief Set to least verbose level (FATAL only)
         */
        void setLeastVerbose()
        {
            setValue(LogLevel::FATAL);
        }

        /**
         * @brief Check if current level allows TRACE logging
         * @return true if TRACE is enabled
         */
        bool isTraceEnabled() const
        {
            return isLevelEnabled(LogLevel::TRACE);
        }

        /**
         * @brief Check if current level allows DEBUG logging
         * @return true if DEBUG is enabled
         */
        bool isDebugEnabled() const
        {
            return isLevelEnabled(LogLevel::DEBUG);
        }

        /**
         * @brief Check if current level allows INFO logging
         * @return true if INFO is enabled
         */
        bool isInfoEnabled() const
        {
            return isLevelEnabled(LogLevel::INFO);
        }

        /**
         * @brief Check if current level allows WARN logging
         * @return true if WARN is enabled
         */
        bool isWarnEnabled() const
        {
            return isLevelEnabled(LogLevel::WARN);
        }

        /**
         * @brief Check if current level allows ERROR logging
         * @return true if ERROR is enabled
         */
        bool isErrorEnabled() const
        {
            return isLevelEnabled(LogLevel::ERROR);
        }

        /**
         * @brief Check if current level allows FATAL logging
         * @return true if FATAL is enabled
         */
        bool isFatalEnabled() const
        {
            return isLevelEnabled(LogLevel::FATAL);
        }
    };

} // namespace MediaDedup
