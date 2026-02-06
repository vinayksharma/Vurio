#pragma once

#include <string>
#include <vector>
#include <algorithm>

namespace MediaDedup
{

    /**
     * @brief Log level enumeration
     *
     * Defines the available log levels in order of increasing severity.
     * Lower levels include higher levels (e.g., DEBUG includes INFO, WARN, ERROR).
     */
    enum class LogLevel
    {
        TRACE = 0, // Most verbose - trace execution flow
        DEBUG = 1, // Debug information for developers
        INFO = 2,  // General information about program execution
        WARN = 3,  // Warning messages for potentially harmful situations
        ERROR = 4, // Error messages for error conditions
        FATAL = 5  // Fatal errors that cause program termination
    };

    /**
     * @brief Convert LogLevel to string
     * @param level Log level to convert
     * @return String representation of the log level
     */
    inline std::string logLevelToString(LogLevel level)
    {
        switch (level)
        {
        case LogLevel::TRACE:
            return "trace";
        case LogLevel::DEBUG:
            return "debug";
        case LogLevel::INFO:
            return "info";
        case LogLevel::WARN:
            return "warn";
        case LogLevel::ERROR:
            return "error";
        case LogLevel::FATAL:
            return "fatal";
        default:
            return "unknown";
        }
    }

    /**
     * @brief Convert string to LogLevel
     * @param str String to convert
     * @return LogLevel value, or LogLevel::INFO as default
     */
    inline LogLevel stringToLogLevel(const std::string &str)
    {
        std::string lower = str;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        if (lower == "trace")
            return LogLevel::TRACE;
        if (lower == "debug")
            return LogLevel::DEBUG;
        if (lower == "info")
            return LogLevel::INFO;
        if (lower == "warn")
            return LogLevel::WARN;
        if (lower == "error")
            return LogLevel::ERROR;
        if (lower == "fatal")
            return LogLevel::FATAL;

        // Default to INFO if string is not recognized
        return LogLevel::INFO;
    }

    /**
     * @brief Check if a log level is enabled given a minimum level
     * @param level Log level to check
     * @param min_level Minimum level required
     * @return true if level is enabled, false otherwise
     */
    inline bool isLogLevelEnabled(LogLevel level, LogLevel min_level)
    {
        return static_cast<int>(level) >= static_cast<int>(min_level);
    }

    /**
     * @brief Get all available log level strings
     * @return Vector of all log level strings
     */
    inline std::vector<std::string> getAllLogLevels()
    {
        return {"trace", "debug", "info", "warn", "error", "fatal"};
    }

    /**
     * @brief Get log level description
     * @param level Log level
     * @return Human-readable description
     */
    inline std::string getLogLevelDescription(LogLevel level)
    {
        switch (level)
        {
        case LogLevel::TRACE:
            return "Trace - Most verbose logging for execution flow";
        case LogLevel::DEBUG:
            return "Debug - Detailed information for debugging";
        case LogLevel::INFO:
            return "Info - General information about program execution";
        case LogLevel::WARN:
            return "Warning - Potentially harmful situations";
        case LogLevel::ERROR:
            return "Error - Error conditions that don't stop execution";
        case LogLevel::FATAL:
            return "Fatal - Fatal errors that cause program termination";
        default:
            return "Unknown log level";
        }
    }

} // namespace MediaDedup
