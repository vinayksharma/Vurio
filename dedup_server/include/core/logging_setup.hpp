#pragma once

#include <Poco/Logger.h>
#include <Poco/Channel.h>
#include <Poco/Formatter.h>
#include <Poco/AutoPtr.h>
#include <string>

namespace MediaDedup
{
    /**
     * @brief Handles logging setup and color formatting for the server
     */
    class LoggingSetup
        {
        public:
            /**
             * @brief Constructor
             */
            LoggingSetup();

            /**
             * @brief Destructor
             */
            ~LoggingSetup() = default;

            /**
             * @brief Set up color-coded logging for different log levels
             */
            void setupColorLogging();

            /**
             * @brief Apply logging level to Poco logger based on string value
             * @param level Log level string (trace, debug, info, warn, error, etc.)
             */
            void applyLogLevel(const std::string &level);

        private:
            Poco::Logger &logger_;

            /**
             * @brief Check if terminal supports colors
             * @return true if colors are supported, false otherwise
             */
            bool supportsColors() const;
    };
}
