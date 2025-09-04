#pragma once

#include <Poco/Util/ServerApplication.h>
#include <Poco/Util/Option.h>
#include <Poco/Util/OptionSet.h>
#include <Poco/Util/HelpFormatter.h>
#include <Poco/Logger.h>
#include <Poco/Net/HTTPServer.h>
#include <Poco/Net/HTTPServerParams.h>
#include <Poco/Net/ServerSocket.h>
#include <Poco/Net/SocketAddress.h>
#include <memory>
#include <string>

// Include the actual headers instead of forward declarations
#include "config/unified_observable_config.hpp"
#include "database/database_manager.hpp"
#include "core/web_server.hpp"
#include "core/console_input_manager.hpp"

namespace MediaDedup
{

    /**
     * @brief Main server application for the media deduplication server
     *
     * This class extends Poco::Util::ServerApplication to provide:
     * - Command-line argument parsing
     * - Configuration management
     * - Database initialization
     * - HTTP server setup
     * - Graceful shutdown handling
     */
    class MediaDedupServer : public Poco::Util::ServerApplication
    {
    public:
        /**
         * @brief Constructor
         */
        MediaDedupServer();

        /**
         * @brief Destructor
         */
        ~MediaDedupServer() override = default;

    protected:
        /**
         * @brief Initialize the application
         * @param self Reference to self
         */
        void initialize(Application &self) override;

        /**
         * @brief Uninitialize the application
         */
        void uninitialize() override;

        /**
         * @brief Define command line options
         * @param options Option set to populate
         */
        void defineOptions(Poco::Util::OptionSet &options) override;

        /**
         * @brief Handle command line options
         * @param name Option name
         * @param value Option value
         */
        void handleOption(const std::string &name, const std::string &value) override;

        /**
         * @brief Display help information
         */
        void handleHelp(const std::string &name, const std::string &value);

        /**
         * @brief Main application logic
         * @return Exit code
         */
        int main(const std::vector<std::string> &args) override;

    private:
        // Configuration and components
        std::shared_ptr<UnifiedObservableConfigManager> config_manager_;
        std::unique_ptr<DatabaseManager> database_manager_;

        // Server components
        std::unique_ptr<WebServer> web_server_;

        // Console input management
        ::MediaDedupServer::Core::ConsoleInputManager &console_input_manager_;
        size_t console_subscription_id_;

        // Configuration options
        std::string config_file_;
        std::string database_path_;
        std::string server_host_;
        uint16_t server_port_;
        bool help_requested_;
        bool daemon_mode_;

        // Logging
        Poco::Logger &logger_;

        /**
         * @brief Initialize configuration
         * @return true if successful, false otherwise
         */
        bool initializeConfiguration();

        /**
         * @brief Initialize database
         * @return true if successful, false otherwise
         */
        bool initializeDatabase();

        /**
         * @brief Initialize web server
         * @return true if successful, false otherwise
         */
        bool initializeWebServer();

        /**
         * @brief Setup request handlers
         */
        void setupRequestHandlers();

        /**
         * @brief Start HTTP server
         * @return true if successful, false otherwise
         */
        bool startHTTPServer();

        /**
         * @brief Stop HTTP server
         */
        void stopHTTPServer();

        /**
         * @brief Wait for shutdown signal
         */
        void waitForShutdown();

        /**
         * @brief Handle shutdown gracefully
         */
        void handleShutdown();

        /**
         * @brief Handle console events
         * @param event Console event to handle
         */
        void handleConsoleEvent(const ::MediaDedupServer::Core::ConsoleEvent &event);

        /**
         * @brief Log server startup information
         */
        void logStartupInfo();

        /**
         * @brief Log server shutdown information
         */
        void logShutdownInfo();
    };

} // namespace MediaDedup
