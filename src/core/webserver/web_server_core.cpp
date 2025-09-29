/*
 * File: core/webserver/web_server_core.cpp
 * Purpose: Implements WebServer and ConfigRequestHandlerFactory routing.
 * Summary:
 *   - Routes /api/* to C++ handlers and other paths to StaticFileHandler
 *   - Manages Poco HTTPServer lifecycle (start/stop threads)
 *   - Integrates with config manager and services (TPM, user settings)
 */
#include "core/web/web_server.hpp"
#include "core/web/static_file_handler.hpp"
#include "core/web/web_handlers_server_status.hpp"
#include "config/unified_observable_config.hpp"
#include "orchestration/thread_pool_manager.hpp"
#include "orchestration/scheduler_service.hpp"
#include "database/user_settings_service.hpp"
#include <Poco/Net/ServerSocket.h>
#include <Poco/Net/SocketAddress.h>
#include <Poco/Net/HTTPServer.h>
#include <Poco/File.h>
#include <Poco/FileStream.h>
#include <Poco/StreamCopier.h>
#include <iostream>

namespace MediaDedup
{

    // Scheduler Job Trigger Handler
    class TriggerJobHandler : public Poco::Net::HTTPRequestHandler
    {
    public:
        TriggerJobHandler(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                         std::shared_ptr<Orchestration::SchedulerService> scheduler_service,
                         const std::string &jobId)
            : config_manager_(std::move(config_manager)),
              scheduler_service_(std::move(scheduler_service)),
              job_id_(jobId) {}

        void handleRequest(Poco::Net::HTTPServerRequest &request, Poco::Net::HTTPServerResponse &response) override;

    private:
        std::shared_ptr<UnifiedObservableConfigManager> config_manager_;
        std::shared_ptr<Orchestration::SchedulerService> scheduler_service_;
        std::string job_id_;
    };

    // Scheduler Status Handler
    class SchedulerStatusHandler : public Poco::Net::HTTPRequestHandler
    {
    public:
        SchedulerStatusHandler(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                              std::shared_ptr<Orchestration::SchedulerService> scheduler_service)
            : config_manager_(std::move(config_manager)),
              scheduler_service_(std::move(scheduler_service)) {}

        void handleRequest(Poco::Net::HTTPServerRequest &request, Poco::Net::HTTPServerResponse &response) override;

    private:
        std::shared_ptr<UnifiedObservableConfigManager> config_manager_;
        std::shared_ptr<Orchestration::SchedulerService> scheduler_service_;
    };

    // Factory
    ConfigRequestHandlerFactory::ConfigRequestHandlerFactory(
        std::shared_ptr<UnifiedObservableConfigManager> config_manager,
        std::shared_ptr<WebServer> web_server,
        std::shared_ptr<UserSettingsService> user_settings_service,
        std::shared_ptr<FilesService> files_service,
        std::shared_ptr<ScannedFilesService> scanned_files_service,
        std::shared_ptr<ThreadPoolManager> tpm,
        std::shared_ptr<Orchestration::SchedulerService> scheduler_service,
        const std::string &web_root_path)
        : config_manager_(std::move(config_manager)),
          web_server_(std::move(web_server)),
          user_settings_service_(std::move(user_settings_service)),
          files_service_(std::move(files_service)),
          scanned_files_service_(std::move(scanned_files_service)),
          tpm_(std::move(tpm)),
          scheduler_service_(std::move(scheduler_service)),
          web_root_path_(web_root_path) {}

    Poco::Net::HTTPRequestHandler *ConfigRequestHandlerFactory::createRequestHandler(
        const Poco::Net::HTTPServerRequest &request)
    {
        const std::string &uri = request.getURI();
        const std::string &method = request.getMethod();

        // Root endpoint - serve Swagger UI
        if (uri == "/" && method == "GET")
            return new SwaggerUIHandler(web_root_path_);

        // Swagger UI assets
        if (uri.find("/swagger-ui/") == 0)
            return new StaticFileHandler(web_root_path_);

        // API endpoints - these need C++ handlers for dynamic data
        if (uri.find("/api/") == 0)
        {
            return createApiHandler(uri, method);
        }

        // Everything else (HTML, CSS, JS, images) served as static files
        return new StaticFileHandler(web_root_path_);
    }

    Poco::Net::HTTPRequestHandler *ConfigRequestHandlerFactory::createApiHandler(
        const std::string &uri, const std::string &method)
    {
        // Dynamic API endpoints that require C++ handlers
        if (uri == "/api/v1/config" && method == "GET")
            return new GetAllConfigHandler(config_manager_);
        if (uri == "/api/v1/config/reload" && method == "POST")
            return new ReloadConfigHandler(config_manager_);
        if (uri == "/api/v1/server/status" && method == "GET")
            return new ServerStatusHandler(config_manager_, files_service_, scanned_files_service_, tpm_);
        if (uri == "/api/v1/config/restart-webserver" && method == "POST")
            return new RestartWebServerHandler(config_manager_, web_server_);

        if (uri.find("/api/v1/config/") == 0)
        {
            if (method == "GET")
                return new GetConfigPropertyHandler(config_manager_);
            if (method == "PUT")
                return new UpdateConfigPropertyHandler(config_manager_);
        }

        if (uri == "/api/v1/user-settings" && method == "GET")
            return new ListUserSettingsHandler(config_manager_, user_settings_service_);
        if (uri.find("/api/v1/user-settings/") == 0)
        {
            if (method == "GET")
                return new GetUserSettingHandler(config_manager_, user_settings_service_);
            if (method == "PUT")
                return new PutUserSettingHandler(config_manager_, user_settings_service_);
            if (method == "DELETE")
                return new DeleteUserSettingHandler(config_manager_, user_settings_service_);
        }

        if (uri == "/api/v1/media-locations/register" && method == "POST")
            return new RegisterMediaLocationHandler(config_manager_, files_service_);
        if (uri == "/api/v1/media-locations/deregister" && method == "POST")
            return new DeregisterMediaLocationHandler(config_manager_, files_service_);

        // Scheduler management endpoints
        if (uri.find("/api/v1/scheduler/trigger/") == 0 && method == "POST")
        {
            std::string jobId = uri.substr(25); // Remove "/api/v1/scheduler/trigger/"
            return new TriggerJobHandler(config_manager_, scheduler_service_, jobId);
        }
        if (uri == "/api/v1/scheduler/status" && method == "GET")
            return new SchedulerStatusHandler(config_manager_, scheduler_service_);

        // Static API responses served as files
        if (uri == "/api/openapi.json" && method == "GET")
            return new StaticFileHandler(web_root_path_);
        if (uri == "/api/endpoints" && method == "GET")
            return new StaticFileHandler(web_root_path_);

        return nullptr;
    }

    // WebServer
    WebServer::WebServer(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                         const std::string &host, uint16_t port,
                         std::shared_ptr<Orchestration::SchedulerService> scheduler_service)
        : config_manager_(std::move(config_manager)), host_(host), port_(port), running_(false),
          scheduler_service_(std::move(scheduler_service))
    {
    }

    WebServer::~WebServer() { stop(); }

    bool WebServer::start()
    {
        try
        {
            if (running_)
                return true;
            // Set running before starting the server thread to avoid a race
            running_ = true;
            initializeServer();
            return true;
        }
        catch (const std::exception &e)
        {
            std::cerr << "Failed to start web server: " << e.what() << std::endl;
            return false;
        }
    }

    void WebServer::stop()
    {
        if (!running_)
            return;
        running_ = false;
        if (server_thread_.joinable())
            server_thread_.join();
        http_server_.reset();
        server_socket_.reset();
    }

    void WebServer::initializeServer()
    {
        Poco::Net::SocketAddress socket_address(host_, port_);
        server_socket_ = std::make_unique<Poco::Net::ServerSocket>(socket_address);

        auto factory = std::make_unique<ConfigRequestHandlerFactory>(config_manager_,
                                                                     std::shared_ptr<WebServer>(this, [](WebServer *) {}),
                                                                     user_settings_service_,
                                                                     files_service_,
                                                                     scanned_files_service_,
                                                                     tpm_,
                                                                     scheduler_service_,
                                                                     "src/core/webserver/static/");

        http_server_ = std::make_unique<Poco::Net::HTTPServer>(
            Poco::Net::HTTPRequestHandlerFactory::Ptr(factory.release()), *server_socket_, new Poco::Net::HTTPServerParams);

        server_thread_ = std::thread([this]()
                                     {
        try {
            http_server_->start();
            std::cout << "Web server started on " << host_ << ":" << port_ << std::endl;
            while (running_) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            http_server_->stop();
        } catch (const std::exception &e) {
            std::cerr << "Web server error: " << e.what() << std::endl;
        } });
    }

    void WebServer::cleanupServer()
    {
        if (server_thread_.joinable())
            server_thread_.join();
        http_server_.reset();
    }

    bool WebServer::restart()
    {
        try
        {
            if (running_)
            {
                stop();
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            return start();
        }
        catch (const std::exception &e)
        {
            std::cerr << "Failed to restart web server: " << e.what() << std::endl;
            return false;
        }
    }

    bool WebServer::restartWithNewConfig()
    {
        try
        {
            if (!config_manager_)
                return false;
            std::string new_host = config_manager_->getPropertyValue<std::string>("server.host", host_);
            uint16_t new_port = config_manager_->getPropertyValue<uint16_t>("server.port", port_);
            if (new_host == host_ && new_port == port_)
                return true;
            host_ = new_host;
            port_ = new_port;
            return restart();
        }
        catch (const std::exception &e)
        {
            std::cerr << "Failed to restart web server with new config: " << e.what() << std::endl;
            return false;
        }
    }

    std::string WebServer::getStatus() const
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        std::stringstream ss;
        ss << "Web Server Status:\n";
        ss << "  Host: " << host_ << "\n";
        ss << "  Port: " << port_ << "\n";
        ss << "  Running: " << (running_ ? "yes" : "no") << "\n";
        return ss.str();
    }

    // SwaggerUIHandler implementation
    SwaggerUIHandler::SwaggerUIHandler(const std::string &web_root_path)
        : web_root_path_(web_root_path)
    {
    }

    void SwaggerUIHandler::handleRequest(Poco::Net::HTTPServerRequest &request,
                                         Poco::Net::HTTPServerResponse &response)
    {
        if (request.getMethod() != "GET")
        {
            response.setStatusAndReason(Poco::Net::HTTPResponse::HTTP_METHOD_NOT_ALLOWED, "Method Not Allowed");
            response.send();
            return;
        }

        try
        {
            // Set response headers
            response.setStatus(Poco::Net::HTTPResponse::HTTP_OK);
            response.setContentType("text/html; charset=utf-8");
            response.set("Cache-Control", "no-cache, no-store, must-revalidate");
            response.set("Pragma", "no-cache");
            response.set("Expires", "0");

            // Read the Swagger UI HTML file
            std::string swagger_ui_path = web_root_path_ + "swagger-ui/index.html";
            Poco::File swagger_ui_file(swagger_ui_path);

            if (!swagger_ui_file.exists())
            {
                response.setStatusAndReason(Poco::Net::HTTPResponse::HTTP_NOT_FOUND, "Swagger UI not found");
                response.send();
                return;
            }

            Poco::FileInputStream fis(swagger_ui_path);
            Poco::StreamCopier::copyStream(fis, response.send());
        }
        catch (const std::exception &e)
        {
            response.setStatusAndReason(Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR, "Internal Server Error");
            response.send();
        }
    }

    // TriggerJobHandler implementation
    void TriggerJobHandler::handleRequest(Poco::Net::HTTPServerRequest &request, Poco::Net::HTTPServerResponse &response)
    {
        Poco::Logger &logger = Poco::Logger::get("TriggerJobHandler");
        
        try
        {
            if (!scheduler_service_)
            {
                response.setStatusAndReason(Poco::Net::HTTPResponse::HTTP_SERVICE_UNAVAILABLE, "Scheduler service not available");
                response.send();
                return;
            }

            bool success = scheduler_service_->triggerJob(job_id_);
            bool isRunning = scheduler_service_->isJobRunning(job_id_);
            
            response.setContentType("application/json");
            response.setStatusAndReason(Poco::Net::HTTPResponse::HTTP_OK, "OK");
            
            std::string status_str = success ? "success" : (isRunning ? "skipped" : "error");
            std::string state_str = isRunning ? "RUNNING" : "IDLE";
            std::string message_str = success ? "Job triggered successfully" : (isRunning ? "Job already running" : "Job not found");
            
            std::string json = "{"
                "\"status\":\"" + status_str + "\","
                "\"job_id\":\"" + job_id_ + "\","
                "\"job_state\":\"" + state_str + "\","
                "\"message\":\"" + message_str + "\""
                "}";
            
            response.sendBuffer(json.data(), json.size());
            logger.debug("Job trigger request for %s: %s", job_id_, success ? "success" : "skipped");
        }
        catch (const std::exception &e)
        {
            logger.error("Error triggering job %s: %s", job_id_, std::string(e.what()));
            response.setStatusAndReason(Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR, "Internal Server Error");
            response.send();
        }
    }

    // SchedulerStatusHandler implementation
    void SchedulerStatusHandler::handleRequest(Poco::Net::HTTPServerRequest &request, Poco::Net::HTTPServerResponse &response)
    {
        Poco::Logger &logger = Poco::Logger::get("SchedulerStatusHandler");
        
        try
        {
            if (!scheduler_service_)
            {
                response.setStatusAndReason(Poco::Net::HTTPResponse::HTTP_SERVICE_UNAVAILABLE, "Scheduler service not available");
                response.send();
                return;
            }

            auto statuses = scheduler_service_->getJobStatuses();
            
            response.setContentType("application/json");
            response.setStatusAndReason(Poco::Net::HTTPResponse::HTTP_OK, "OK");
            
            std::string json = "{\"jobs\":[";
            for (size_t i = 0; i < statuses.size(); ++i)
            {
                if (i > 0) json += ",";
                
                const auto& status = statuses[i];
                auto lastRunTime = std::chrono::duration_cast<std::chrono::seconds>(
                    status.lastRun.time_since_epoch()).count();
                auto nextRunTime = std::chrono::duration_cast<std::chrono::seconds>(
                    status.nextRun.time_since_epoch()).count();
                
                json += "{"
                    "\"job_id\":\"" + status.jobId + "\","
                    "\"state\":\"" + (status.state == Orchestration::JobState::RUNNING ? "RUNNING" : "IDLE") + "\","
                    "\"interval_ms\":" + std::to_string(status.interval.count()) + ","
                    "\"last_run\":" + std::to_string(lastRunTime) + ","
                    "\"next_run\":" + std::to_string(nextRunTime) + ","
                    "\"consecutive_failures\":" + std::to_string(status.consecutiveFailures) + ","
                    "\"total_failures\":" + std::to_string(status.totalFailures) +
                    "}";
            }
            json += "]}";
            
            response.sendBuffer(json.data(), json.size());
            logger.debug("Scheduler status request completed");
        }
        catch (const std::exception &e)
        {
            logger.error("Error getting scheduler status: %s", std::string(e.what()));
            response.setStatusAndReason(Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR, "Internal Server Error");
            response.send();
        }
    }

} // namespace MediaDedup
