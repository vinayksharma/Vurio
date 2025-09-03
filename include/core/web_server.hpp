#pragma once

#include <string>
#include <memory>
#include <functional>
#include <unordered_map>
#include <thread>
#include <atomic>
#include <mutex>

// Include Poco headers
#include <Poco/Net/HTTPServer.h>
#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPRequestHandlerFactory.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>

namespace MediaDedup
{
    class UnifiedObservableConfigManager;

    /**
     * @brief Request handler factory for routing HTTP requests
     */
    class ConfigRequestHandlerFactory : public Poco::Net::HTTPRequestHandlerFactory
    {
    public:
        ConfigRequestHandlerFactory(std::shared_ptr<UnifiedObservableConfigManager> config_manager);

        Poco::Net::HTTPRequestHandler *createRequestHandler(const Poco::Net::HTTPServerRequest &request) override;

    private:
        std::shared_ptr<UnifiedObservableConfigManager> config_manager_;
    };

    /**
     * @brief Web server for configuration management via HTTP API
     *
     * Provides RESTful endpoints for:
     * - GET /api/v1/config - Retrieve all configuration
     * - GET /api/v1/config/{key} - Retrieve specific configuration property
     * - PUT /api/v1/config/{key} - Update specific configuration property
     * - POST /api/v1/config/reload - Reload configuration from file
     * - GET /api/v1/config/status - Get configuration system status
     * - GET /api/openapi.json - OpenAPI specification
     */
    class WebServer
    {
    public:
        using ConfigUpdateCallback = std::function<bool(const std::string &, const std::string &)>;

        WebServer(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                  const std::string &host = "0.0.0.0",
                  uint16_t port = 8080);

        ~WebServer();

        // Server lifecycle
        bool start();
        void stop();
        bool isRunning() const { return running_; }

        // Configuration
        void setHost(const std::string &host) { host_ = host; }
        void setPort(uint16_t port) { port_ = port; }
        std::string getHost() const { return host_; }
        uint16_t getPort() const { return port_; }

        // Callbacks
        void setConfigUpdateCallback(ConfigUpdateCallback callback) { config_update_callback_ = callback; }

        // Status
        std::string getStatus() const;

    private:
        std::shared_ptr<UnifiedObservableConfigManager> config_manager_;
        std::string host_;
        uint16_t port_;
        std::atomic<bool> running_;

        std::unique_ptr<Poco::Net::HTTPServer> http_server_;
        std::thread server_thread_;

        ConfigUpdateCallback config_update_callback_;
        mutable std::mutex status_mutex_;

        // Private methods
        void initializeServer();
        void cleanupServer();
    };

    /**
     * @brief Base HTTP request handler for configuration endpoints
     */
    class ConfigRequestHandler : public Poco::Net::HTTPRequestHandler
    {
    public:
        ConfigRequestHandler(std::shared_ptr<UnifiedObservableConfigManager> config_manager);

        void handleRequest(Poco::Net::HTTPServerRequest &request,
                           Poco::Net::HTTPServerResponse &response) override;

    protected:
        std::shared_ptr<UnifiedObservableConfigManager> config_manager_;

        // Helper methods
        void sendJsonResponse(Poco::Net::HTTPServerResponse &response,
                              const std::string &json_data,
                              int status_code = 200);
        void sendErrorResponse(Poco::Net::HTTPServerResponse &response,
                               const std::string &error_message,
                               int status_code = 400);
        std::string getRequestBody(Poco::Net::HTTPServerRequest &request);
        std::string extractKeyFromPath(const std::string &path);
    };

    /**
     * @brief Handler for GET /api/v1/config (all configuration)
     */
    class GetAllConfigHandler : public ConfigRequestHandler
    {
    public:
        using ConfigRequestHandler::ConfigRequestHandler;

        void handleRequest(Poco::Net::HTTPServerRequest &request,
                           Poco::Net::HTTPServerResponse &response) override;
    };

    /**
     * @brief Handler for GET /api/v1/config/{key} (specific property)
     */
    class GetConfigPropertyHandler : public ConfigRequestHandler
    {
    public:
        using ConfigRequestHandler::ConfigRequestHandler;

        void handleRequest(Poco::Net::HTTPServerRequest &request,
                           Poco::Net::HTTPServerResponse &response) override;
    };

    /**
     * @brief Handler for PUT /api/v1/config/{key} (update property)
     */
    class UpdateConfigPropertyHandler : public ConfigRequestHandler
    {
    public:
        using ConfigRequestHandler::ConfigRequestHandler;

        void handleRequest(Poco::Net::HTTPServerRequest &request,
                           Poco::Net::HTTPServerResponse &response) override;
    };

    /**
     * @brief Handler for POST /api/v1/config/reload (reload configuration)
     */
    class ReloadConfigHandler : public ConfigRequestHandler
    {
    public:
        using ConfigRequestHandler::ConfigRequestHandler;

        void handleRequest(Poco::Net::HTTPServerRequest &request,
                           Poco::Net::HTTPServerResponse &response) override;
    };

    /**
     * @brief Handler for GET /api/v1/config/status (system status)
     */
    class ConfigStatusHandler : public ConfigRequestHandler
    {
    public:
        using ConfigRequestHandler::ConfigRequestHandler;

        void handleRequest(Poco::Net::HTTPServerRequest &request,
                           Poco::Net::HTTPServerResponse &response) override;
    };

    /**
     * @brief Handler for GET /api/openapi.json (OpenAPI specification)
     */
    class OpenApiSpecHandler : public ConfigRequestHandler
    {
    public:
        using ConfigRequestHandler::ConfigRequestHandler;

        void handleRequest(Poco::Net::HTTPServerRequest &request,
                           Poco::Net::HTTPServerResponse &response) override;
    };

} // namespace MediaDedup
