#include "core/web_server.hpp"
#include "config/unified_observable_config.hpp"
#include "orchestration/thread_pool_manager.hpp"
#include "database/user_settings_service.hpp"
#include <Poco/Net/ServerSocket.h>
#include <Poco/Net/SocketAddress.h>
#include <Poco/Net/HTTPServer.h>
#include <iostream>

namespace MediaDedup
{

    // Factory
    ConfigRequestHandlerFactory::ConfigRequestHandlerFactory(
        std::shared_ptr<UnifiedObservableConfigManager> config_manager,
        std::shared_ptr<WebServer> web_server,
        std::shared_ptr<UserSettingsService> user_settings_service,
        std::shared_ptr<FilesService> files_service,
        std::shared_ptr<ThreadPoolManager> tpm)
        : config_manager_(std::move(config_manager)),
          web_server_(std::move(web_server)),
          user_settings_service_(std::move(user_settings_service)),
          files_service_(std::move(files_service)),
          tpm_(std::move(tpm)) {}

    Poco::Net::HTTPRequestHandler *ConfigRequestHandlerFactory::createRequestHandler(
        const Poco::Net::HTTPServerRequest &request)
    {
        const std::string &uri = request.getURI();
        const std::string &method = request.getMethod();

        if (uri == "/api/v1/config" && method == "GET")
            return new GetAllConfigHandler(config_manager_);
        if (uri == "/api/v1/config/reload" && method == "POST")
            return new ReloadConfigHandler(config_manager_);
        if (uri == "/api/v1/config/status" && method == "GET")
            return new ConfigStatusHandler(config_manager_);
        if (uri == "/api/v1/tpm/status" && method == "GET")
            return new TPMStatusHandler(config_manager_, tpm_);
        if (uri == "/api/v1/config/restart-webserver" && method == "POST")
            return new RestartWebServerHandler(config_manager_, web_server_);
        if (uri == "/api/openapi.json" && method == "GET")
            return new OpenApiSpecHandler(config_manager_);
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

        return nullptr;
    }

    // WebServer
    WebServer::WebServer(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                         const std::string &host, uint16_t port)
        : config_manager_(std::move(config_manager)), host_(host), port_(port), running_(false) {}

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
    }

    void WebServer::initializeServer()
    {
        Poco::Net::SocketAddress socket_address(host_, port_);
        auto server_socket = std::make_unique<Poco::Net::ServerSocket>(socket_address);

        auto factory = std::make_unique<ConfigRequestHandlerFactory>(config_manager_,
                                                                     std::shared_ptr<WebServer>(this, [](WebServer *) {}),
                                                                     user_settings_service_,
                                                                     files_service_,
                                                                     tpm_);

        http_server_ = std::make_unique<Poco::Net::HTTPServer>(
            Poco::Net::HTTPRequestHandlerFactory::Ptr(factory.release()), *server_socket, new Poco::Net::HTTPServerParams);

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

} // namespace MediaDedup
