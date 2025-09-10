#include "core/logging_setup.hpp"
#include <Poco/ConsoleChannel.h>
#include <Poco/PatternFormatter.h>
#include <Poco/FormattingChannel.h>
#include <Poco/DateTime.h>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <cstdlib>

namespace MediaDedup
{
        LoggingSetup::LoggingSetup()
            : logger_(Poco::Logger::get("LoggingSetup"))
        {
        }

        void LoggingSetup::setupColorLogging()
        {
            // Create a custom formatter that adds colors
            class ColorFormatter : public Poco::Formatter
            {
            public:
                void format(const Poco::Message &msg, std::string &text)
                {
                    // Format the message with timestamp and log level
                    std::ostringstream oss;
                    Poco::DateTime dateTime(msg.getTime());
                    
                    // Convert priority to string
                    std::string priorityStr;
                    switch (msg.getPriority())
                    {
                    case Poco::Message::PRIO_FATAL:
                        priorityStr = "FATAL";
                        break;
                    case Poco::Message::PRIO_CRITICAL:
                        priorityStr = "CRITICAL";
                        break;
                    case Poco::Message::PRIO_ERROR:
                        priorityStr = "ERROR";
                        break;
                    case Poco::Message::PRIO_WARNING:
                        priorityStr = "WARNING";
                        break;
                    case Poco::Message::PRIO_INFORMATION:
                        priorityStr = "INFORMATION";
                        break;
                    case Poco::Message::PRIO_DEBUG:
                        priorityStr = "DEBUG";
                        break;
                    case Poco::Message::PRIO_TRACE:
                        priorityStr = "TRACE";
                        break;
                    default:
                        priorityStr = "UNKNOWN";
                        break;
                    }

                    oss << std::setfill('0')
                        << std::setw(4) << dateTime.year() << "-"
                        << std::setw(2) << dateTime.month() << "-"
                        << std::setw(2) << dateTime.day() << " "
                        << std::setw(2) << dateTime.hour() << ":"
                        << std::setw(2) << dateTime.minute() << ":"
                        << std::setw(2) << dateTime.second() << "."
                        << std::setw(3) << (dateTime.millisecond() / 1000) << " "
                        << "[" << priorityStr << "] "
                        << msg.getSource() << ": "
                        << msg.getText();

                    text = oss.str();
                }
            };

            // Create a custom channel that adds colors
            class ColorChannel : public Poco::Channel
            {
            public:
                void log(const Poco::Message &msg)
                {
                    ColorFormatter formatter;
                    std::string formattedMsg;
                    formatter.format(msg, formattedMsg);

                    // Check if we should use colors
                    bool useColors = false;
                    const char *term = std::getenv("TERM");
                    const char *colorterm = std::getenv("COLORTERM");
                    const char *force_color = std::getenv("FORCE_COLOR");

                    if (term && (std::string(term) == "xterm" || std::string(term) == "xterm-256color" || 
                                std::string(term) == "screen" || std::string(term) == "screen-256color" ||
                                std::string(term).find("color") != std::string::npos))
                    {
                        useColors = true;
                    }
                    if (colorterm || force_color)
                    {
                        useColors = true;
                    }

                    if (useColors)
                    {
                        // Apply colors based on log level
                        std::string colorCode;
                        switch (msg.getPriority())
                        {
                        case Poco::Message::PRIO_FATAL:
                        case Poco::Message::PRIO_CRITICAL:
                            colorCode = "\033[1;31m"; // Bold red
                            break;
                        case Poco::Message::PRIO_ERROR:
                            colorCode = "\033[31m"; // Red
                            break;
                        case Poco::Message::PRIO_WARNING:
                            colorCode = "\033[33m"; // Yellow
                            break;
                        case Poco::Message::PRIO_INFORMATION:
                            colorCode = "\033[32m"; // Green
                            break;
                        case Poco::Message::PRIO_DEBUG:
                            colorCode = "\033[36m"; // Cyan
                            break;
                        case Poco::Message::PRIO_TRACE:
                            colorCode = "\033[37m"; // White
                            break;
                        default:
                            colorCode = "\033[0m"; // Reset
                            break;
                        }

                        std::cout << colorCode << formattedMsg << "\033[0m" << std::endl;
                    }
                    else
                    {
                        // No colors, just output the formatted message
                        std::cout << formattedMsg << std::endl;
                    }
                }
            };

            // Set the custom color channel
            Poco::AutoPtr<Poco::Channel> colorChannel(new ColorChannel);
            Poco::Logger::root().setChannel(colorChannel);
        }

        void LoggingSetup::applyLogLevel(const std::string &level)
        {
            std::string lowered = level;
            for (char &c : lowered)
                c = static_cast<char>(::tolower(c));

            // Map synonyms to Poco names
            std::string pocoLevel = lowered;
            if (lowered == "info")
                pocoLevel = "information";
            else if (lowered == "warn")
                pocoLevel = "warning";

            try
            {
                logger_.setLevel(pocoLevel);
                Poco::Logger::root().setLevel(pocoLevel);
            }
            catch (...)
            {
                logger_.setLevel("information");
                Poco::Logger::root().setLevel("information");
            }
        }

        bool LoggingSetup::supportsColors() const
        {
            const char *term = std::getenv("TERM");
            const char *colorterm = std::getenv("COLORTERM");
            const char *force_color = std::getenv("FORCE_COLOR");

            if (term && (std::string(term) == "xterm" || std::string(term) == "xterm-256color" || 
                        std::string(term) == "screen" || std::string(term) == "screen-256color" ||
                        std::string(term).find("color") != std::string::npos))
            {
                return true;
            }
            if (colorterm || force_color)
            {
                return true;
            }
            return false;
    }
}
