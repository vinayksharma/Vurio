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
#include <Poco/Net/ServerSocket.h>

namespace MediaDedup
{
    class UnifiedObservableConfigManager;
    class WebServer;
    class ThreadPoolManager;
    namespace Orchestration
    {
        class SchedulerService;
    }

    // Forward declarations
    namespace Orchestration
    {
        class DuplicateFinder;
    }

    /**
     * @brief Request handler factory for routing HTTP requests
     */
    class ConfigRequestHandlerFactory : public Poco::Net::HTTPRequestHandlerFactory
    {
    public:
        ConfigRequestHandlerFactory(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                                    std::shared_ptr<WebServer> web_server = nullptr,
                                    std::shared_ptr<class UserSettingsService> user_settings_service = nullptr,
                                    std::shared_ptr<class FilesService> files_service = nullptr,
                                    std::shared_ptr<class ScannedFilesService> scanned_files_service = nullptr,
                                    std::shared_ptr<class ThreadPoolManager> tpm = nullptr,
                                    std::shared_ptr<Orchestration::SchedulerService> scheduler_service = nullptr,
                                    std::shared_ptr<Orchestration::DuplicateFinder> duplicate_finder = nullptr,
                                    std::shared_ptr<class DatabaseManager> database_manager = nullptr,
                                    std::shared_ptr<class DiskCache> thumbnail_cache = nullptr,
                                    const std::string &web_root_path = "src/core/webserver/static/");

        Poco::Net::HTTPRequestHandler *createRequestHandler(const Poco::Net::HTTPServerRequest &request) override;

    private:
        Poco::Net::HTTPRequestHandler *createApiHandler(const std::string &uri, const std::string &method);

        std::shared_ptr<UnifiedObservableConfigManager> config_manager_;
        std::shared_ptr<WebServer> web_server_;
        std::shared_ptr<class UserSettingsService> user_settings_service_;
        std::shared_ptr<class FilesService> files_service_;
        std::shared_ptr<class ScannedFilesService> scanned_files_service_;
        std::shared_ptr<class ThreadPoolManager> tpm_;
        std::shared_ptr<Orchestration::SchedulerService> scheduler_service_;
        std::shared_ptr<Orchestration::DuplicateFinder> duplicate_finder_;
        std::shared_ptr<class DatabaseManager> database_manager_;
        std::shared_ptr<class DiskCache> thumbnail_cache_;
        std::string web_root_path_;
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
                  uint16_t port = 8080,
                  std::shared_ptr<Orchestration::SchedulerService> scheduler_service = nullptr);

        ~WebServer();

        // Server lifecycle
        bool start();
        void stop();
        bool isRunning() const { return running_; }
        bool restart();
        bool restartWithNewConfig();

        // Configuration
        void setHost(const std::string &host) { host_ = host; }
        void setPort(uint16_t port) { port_ = port; }
        std::string getHost() const { return host_; }
        uint16_t getPort() const { return port_; }

        // Callbacks
        void setConfigUpdateCallback(ConfigUpdateCallback callback) { config_update_callback_ = callback; }

        // Status
        std::string getStatus() const;

        // Inject database-backed services
        void setUserSettingsService(std::shared_ptr<class UserSettingsService> service) { user_settings_service_ = std::move(service); }
        void setFilesService(std::shared_ptr<class FilesService> service) { files_service_ = std::move(service); }
        void setScannedFilesService(std::shared_ptr<class ScannedFilesService> service) { scanned_files_service_ = std::move(service); }
        void setThreadPoolManager(std::shared_ptr<class ThreadPoolManager> tpm) { tpm_ = std::move(tpm); }
        void setDuplicateFinder(std::shared_ptr<Orchestration::DuplicateFinder> duplicate_finder) { duplicate_finder_ = std::move(duplicate_finder); }
        void setDatabaseManager(std::shared_ptr<class DatabaseManager> db) { database_manager_ = std::move(db); }
        void setThumbnailCache(std::shared_ptr<class DiskCache> cache) { thumbnail_cache_ = std::move(cache); }

    private:
        std::shared_ptr<UnifiedObservableConfigManager> config_manager_;
        std::string host_;
        uint16_t port_;
        std::atomic<bool> running_;

        std::unique_ptr<Poco::Net::HTTPServer> http_server_;
        std::unique_ptr<Poco::Net::ServerSocket> server_socket_;
        std::thread server_thread_;

        ConfigUpdateCallback config_update_callback_;
        mutable std::mutex status_mutex_;

        // Services
        std::shared_ptr<class UserSettingsService> user_settings_service_;
        std::shared_ptr<class FilesService> files_service_;
        std::shared_ptr<class ScannedFilesService> scanned_files_service_;
        std::shared_ptr<class ThreadPoolManager> tpm_;
        std::shared_ptr<Orchestration::SchedulerService> scheduler_service_;
        std::shared_ptr<Orchestration::DuplicateFinder> duplicate_finder_;
        std::shared_ptr<class DatabaseManager> database_manager_;
        std::shared_ptr<class DiskCache> thumbnail_cache_;

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

    // OpenApiSpecHandler - REMOVED: Now served as static file web/static/api/openapi.json
    // ApiEndpointsHandler - REMOVED: Now served as static file web/static/html/endpoints.html

    /**
     * @brief Handler for POST /api/v1/config/restart-webserver (restart web server)
     */
    class RestartWebServerHandler : public ConfigRequestHandler
    {
    public:
        RestartWebServerHandler(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                                std::shared_ptr<WebServer> web_server);

        void handleRequest(Poco::Net::HTTPServerRequest &request,
                           Poco::Net::HTTPServerResponse &response) override;

    private:
        std::shared_ptr<WebServer> web_server_;
    };

    // -------------------- User Settings Handlers --------------------

    class ListUserSettingsHandler : public ConfigRequestHandler
    {
    public:
        ListUserSettingsHandler(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                                std::shared_ptr<class UserSettingsService> service);

        void handleRequest(Poco::Net::HTTPServerRequest &request,
                           Poco::Net::HTTPServerResponse &response) override;

    private:
        std::shared_ptr<class UserSettingsService> service_;
    };

    class GetUserSettingHandler : public ConfigRequestHandler
    {
    public:
        GetUserSettingHandler(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                              std::shared_ptr<class UserSettingsService> service);

        void handleRequest(Poco::Net::HTTPServerRequest &request,
                           Poco::Net::HTTPServerResponse &response) override;

    private:
        std::shared_ptr<class UserSettingsService> service_;
        static std::string extractUserKey(const std::string &path);
    };

    class PutUserSettingHandler : public ConfigRequestHandler
    {
    public:
        PutUserSettingHandler(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                              std::shared_ptr<class UserSettingsService> service);

        void handleRequest(Poco::Net::HTTPServerRequest &request,
                           Poco::Net::HTTPServerResponse &response) override;

    private:
        std::shared_ptr<class UserSettingsService> service_;
        static std::string extractUserKey(const std::string &path);
    };

    class DeleteUserSettingHandler : public ConfigRequestHandler
    {
    public:
        DeleteUserSettingHandler(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                                 std::shared_ptr<class UserSettingsService> service);

        void handleRequest(Poco::Net::HTTPServerRequest &request,
                           Poco::Net::HTTPServerResponse &response) override;

    private:
        std::shared_ptr<class UserSettingsService> service_;
        static std::string extractUserKey(const std::string &path);
    };

    // Media locations handlers
    class RegisterMediaLocationHandler : public ConfigRequestHandler
    {
    public:
        RegisterMediaLocationHandler(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                                     std::shared_ptr<class FilesService> service);

        void handleRequest(Poco::Net::HTTPServerRequest &request,
                           Poco::Net::HTTPServerResponse &response) override;

    private:
        std::shared_ptr<class FilesService> service_;
    };

    class DeregisterMediaLocationHandler : public ConfigRequestHandler
    {
    public:
        DeregisterMediaLocationHandler(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                                       std::shared_ptr<class FilesService> service);

        void handleRequest(Poco::Net::HTTPServerRequest &request,
                           Poco::Net::HTTPServerResponse &response) override;

    private:
        std::shared_ptr<class FilesService> service_;
    };

    // TPM status handler
    class TPMStatusHandler : public ConfigRequestHandler
    {
    public:
        TPMStatusHandler(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                         std::shared_ptr<class ThreadPoolManager> tpm);

        void handleRequest(Poco::Net::HTTPServerRequest &request,
                           Poco::Net::HTTPServerResponse &response) override;

    private:
        std::shared_ptr<class ThreadPoolManager> tpm_;
    };

    /**
     * @brief Handler for serving Swagger UI at the root endpoint
     */
    class SwaggerUIHandler : public Poco::Net::HTTPRequestHandler
    {
    public:
        /**
         * @brief Constructor
         * @param web_root_path Path to the web static files directory
         */
        explicit SwaggerUIHandler(const std::string &web_root_path);

        /**
         * @brief Handle HTTP request for Swagger UI
         * @param request The HTTP request
         * @param response The HTTP response
         */
        void handleRequest(Poco::Net::HTTPServerRequest &request,
                           Poco::Net::HTTPServerResponse &response) override;

    private:
        std::string web_root_path_;
    };

} // namespace MediaDedup
