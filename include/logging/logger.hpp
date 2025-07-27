#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <memory>

class Logger
{
public:
    enum class Level
    {
        TRACE,
        DEBUG,
        INFO,
        WARN,
        ERROR
    };

    static void init(const std::string &log_level = "INFO")
    {
        auto logger = getLogger();

        // Convert string log level to spdlog level
        spdlog::level::level_enum level;
        if (log_level == "TRACE")
            level = spdlog::level::trace;
        else if (log_level == "DEBUG")
            level = spdlog::level::debug;
        else if (log_level == "INFO")
            level = spdlog::level::info;
        else if (log_level == "WARN")
            level = spdlog::level::warn;
        else if (log_level == "ERROR")
            level = spdlog::level::err;
        else
            level = spdlog::level::info; // Default to INFO

        logger->set_level(level);
    }

    static void trace(const std::string &message)
    {
        log(Level::TRACE, message);
    }

    static void debug(const std::string &message)
    {
        log(Level::DEBUG, message);
    }

    static void info(const std::string &message)
    {
        log(Level::INFO, message);
    }

    static void warn(const std::string &message)
    {
        log(Level::WARN, message);
    }

    static void error(const std::string &message)
    {
        log(Level::ERROR, message);
    }

private:
    static std::shared_ptr<spdlog::logger> getLogger()
    {
        static auto logger = spdlog::stdout_color_mt("dedup_server");
        return logger;
    }

    static void log(Level level, const std::string &message)
    {
        auto logger = getLogger();
        switch (level)
        {
        case Level::TRACE:
            logger->trace(message);
            break;
        case Level::DEBUG:
            logger->debug(message);
            break;
        case Level::INFO:
            logger->info(message);
            break;
        case Level::WARN:
            logger->warn(message);
            break;
        case Level::ERROR:
            logger->error(message);
            break;
        }
    }
};