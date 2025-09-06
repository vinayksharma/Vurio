#include "core/web_server.hpp"
#include "config/unified_observable_config.hpp"
#include "database/user_settings_service.hpp"
#include <Poco/Net/HTTPServer.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPRequestHandlerFactory.h>
#include <Poco/Net/ServerSocket.h>
#include <Poco/Net/SocketAddress.h>
#include <Poco/Util/ServerApplication.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Parser.h>
#include <Poco/StreamCopier.h>
#include <Poco/URI.h>
#include <iostream>
#include <sstream>
#include <regex>

namespace MediaDedup
{

    // ============================================================================
    // ConfigRequestHandlerFactory Implementation
    // ============================================================================

    ConfigRequestHandlerFactory::ConfigRequestHandlerFactory(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                                                             std::shared_ptr<WebServer> web_server,
                                                             std::shared_ptr<UserSettingsService> user_settings_service)
        : config_manager_(config_manager), web_server_(web_server), user_settings_service_(user_settings_service)
    {
    }

    Poco::Net::HTTPRequestHandler *ConfigRequestHandlerFactory::createRequestHandler(const Poco::Net::HTTPServerRequest &request)
    {
        const std::string &uri = request.getURI();
        const std::string &method = request.getMethod();

        // Route requests based on URI and method
        if (uri == "/api/v1/config" && method == "GET")
        {
            return new GetAllConfigHandler(config_manager_);
        }
        else if (uri == "/api/v1/config/reload" && method == "POST")
        {
            return new ReloadConfigHandler(config_manager_);
        }
        else if (uri == "/api/v1/config/status" && method == "GET")
        {
            return new ConfigStatusHandler(config_manager_);
        }
        else if (uri == "/api/v1/config/restart-webserver" && method == "POST")
        {
            return new RestartWebServerHandler(config_manager_, web_server_);
        }
        else if (uri == "/api/openapi.json" && method == "GET")
        {
            return new OpenApiSpecHandler(config_manager_);
        }
        else if (uri.find("/api/v1/config/") == 0)
        {
            // Handle /api/v1/config/{key} endpoints
            if (method == "GET")
            {
                return new GetConfigPropertyHandler(config_manager_);
            }
            else if (method == "PUT")
            {
                return new UpdateConfigPropertyHandler(config_manager_);
            }
        }

        // User settings endpoints
        if (uri == "/api/v1/user-settings" && method == "GET")
        {
            return new ListUserSettingsHandler(config_manager_, user_settings_service_);
        }
        if (uri.find("/api/v1/user-settings/") == 0)
        {
            if (method == "GET")
            {
                return new GetUserSettingHandler(config_manager_, user_settings_service_);
            }
            else if (method == "PUT")
            {
                return new PutUserSettingHandler(config_manager_, user_settings_service_);
            }
            else if (method == "DELETE")
            {
                return new DeleteUserSettingHandler(config_manager_, user_settings_service_);
            }
        }

        // Return nullptr for unmatched routes (will result in 404)
        return nullptr;
    }

    // ============================================================================
    // WebServer Implementation
    // ============================================================================

    WebServer::WebServer(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                         const std::string &host, uint16_t port)
        : config_manager_(config_manager), host_(host), port_(port), running_(false)
    {
    }

    WebServer::~WebServer()
    {
        stop();
    }

    bool WebServer::start()
    {
        try
        {
            if (running_)
            {
                return true; // Already running
            }

            initializeServer();
            running_ = true;
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
        {
            return;
        }

        running_ = false;

        // Wait for server thread to finish (it will call http_server_->stop())
        if (server_thread_.joinable())
        {
            server_thread_.join();
        }

        // Clean up resources
        http_server_.reset();
    }

    void WebServer::initializeServer()
    {
        // Create server socket
        Poco::Net::SocketAddress socket_address(host_, port_);
        auto server_socket = std::make_unique<Poco::Net::ServerSocket>(socket_address);

        // Create request handler factory
        auto factory = std::make_unique<ConfigRequestHandlerFactory>(config_manager_,
                                                                     std::shared_ptr<WebServer>(this, [](WebServer *) {}),
                                                                     user_settings_service_); // Non-owning shared_ptr for WebServer

        // Create HTTP server
        http_server_ = std::make_unique<Poco::Net::HTTPServer>(
            Poco::Net::HTTPRequestHandlerFactory::Ptr(factory.release()),
            *server_socket,
            new Poco::Net::HTTPServerParams);

        // Start server in background thread
        server_thread_ = std::thread([this]()
                                     {
        try {
            http_server_->start();
            std::cout << "Web server started on " << host_ << ":" << port_ << std::endl;
            
            // Wait for stop signal
            while (running_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            
            http_server_->stop();
        } catch (const std::exception& e) {
            std::cerr << "Web server error: " << e.what() << std::endl;
        } });
    }

    void WebServer::cleanupServer()
    {
        // This method is now only used for cleanup during restart
        // The main stop() method handles the primary cleanup logic

        if (server_thread_.joinable())
        {
            server_thread_.join();
        }

        // Reset the HTTP server (it should already be stopped by the server thread)
        http_server_.reset();
    }

    bool WebServer::restart()
    {
        try
        {
            if (running_)
            {
                stop();
                // Wait a moment for cleanup
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
            {
                std::cerr << "No configuration manager available" << std::endl;
                return false;
            }

            // Get new configuration values
            std::string new_host = config_manager_->getPropertyValue<std::string>("server.host", host_);
            uint16_t new_port = config_manager_->getPropertyValue<uint16_t>("server.port", port_);

            // Check if configuration actually changed
            if (new_host == host_ && new_port == port_)
            {
                std::cout << "Web server configuration unchanged, no restart needed" << std::endl;
                return true;
            }

            std::cout << "Web server configuration changed:" << std::endl;
            std::cout << "  Host: " << host_ << " -> " << new_host << std::endl;
            std::cout << "  Port: " << port_ << " -> " << new_port << std::endl;

            // Update configuration
            host_ = new_host;
            port_ = new_port;

            // Restart with new configuration
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

    // ============================================================================
    // ConfigRequestHandler Implementation
    // ============================================================================

    ConfigRequestHandler::ConfigRequestHandler(std::shared_ptr<UnifiedObservableConfigManager> config_manager)
        : config_manager_(config_manager)
    {
    }

    void ConfigRequestHandler::handleRequest(Poco::Net::HTTPServerRequest &request,
                                             Poco::Net::HTTPServerResponse &response)
    {
        // Set CORS headers
        response.set("Access-Control-Allow-Origin", "*");
        response.set("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        response.set("Access-Control-Allow-Headers", "Content-Type, Authorization");

        // Handle preflight OPTIONS request
        if (request.getMethod() == "OPTIONS")
        {
            response.setStatusAndReason(Poco::Net::HTTPResponse::HTTP_OK);
            response.send();
            return;
        }

        // Default implementation - subclasses should override
        sendErrorResponse(response, "Method not implemented", 501);
    }

    void ConfigRequestHandler::sendJsonResponse(Poco::Net::HTTPServerResponse &response,
                                                const std::string &json_data, int status_code)
    {
        response.setStatusAndReason(static_cast<Poco::Net::HTTPResponse::HTTPStatus>(status_code));
        response.setContentType("application/json");
        response.setContentLength(json_data.length());

        std::ostream &ostr = response.send();
        ostr << json_data;
    }

    void ConfigRequestHandler::sendErrorResponse(Poco::Net::HTTPServerResponse &response,
                                                 const std::string &error_message, int status_code)
    {
        Poco::JSON::Object error_obj;
        error_obj.set("error", error_message);
        error_obj.set("status", status_code);

        std::stringstream ss;
        error_obj.stringify(ss);

        sendJsonResponse(response, ss.str(), status_code);
    }

    std::string ConfigRequestHandler::getRequestBody(Poco::Net::HTTPServerRequest &request)
    {
        std::string body;
        Poco::StreamCopier::copyToString(request.stream(), body);
        return body;
    }

    std::string ConfigRequestHandler::extractKeyFromPath(const std::string &path)
    {
        // Extract key from path like /api/v1/config/{key}
        std::regex path_regex(R"(/api/v1/config/(.+))");
        std::smatch match;

        if (std::regex_match(path, match, path_regex) && match.size() > 1)
        {
            return match[1].str();
        }

        return "";
    }

    // ============================================================================
    // GetAllConfigHandler Implementation
    // ============================================================================

    void GetAllConfigHandler::handleRequest(Poco::Net::HTTPServerRequest &request,
                                            Poco::Net::HTTPServerResponse &response)
    {
        if (request.getMethod() != "GET")
        {
            sendErrorResponse(response, "Method not allowed", 405);
            return;
        }

        try
        {
            Poco::JSON::Object config_obj;

            // Get all property keys
            auto keys = config_manager_->getAllPropertyKeys();

            for (const auto &key : keys)
            {
                auto property = config_manager_->getProperty<std::any>(key);
                if (property)
                {
                    // Convert property value to JSON
                    auto value = property->getValue();
                    if (value.type() == typeid(std::string))
                    {
                        config_obj.set(key, std::any_cast<std::string>(value));
                    }
                    else if (value.type() == typeid(int))
                    {
                        config_obj.set(key, std::any_cast<int>(value));
                    }
                    else if (value.type() == typeid(double))
                    {
                        config_obj.set(key, std::any_cast<double>(value));
                    }
                    else if (value.type() == typeid(bool))
                    {
                        config_obj.set(key, std::any_cast<bool>(value));
                    }
                    else
                    {
                        config_obj.set(key, property->getValueAsString());
                    }
                }
            }

            std::stringstream ss;
            config_obj.stringify(ss);
            sendJsonResponse(response, ss.str());
        }
        catch (const std::exception &e)
        {
            sendErrorResponse(response, "Failed to retrieve configuration: " + std::string(e.what()), 500);
        }
    }

    // ============================================================================
    // GetConfigPropertyHandler Implementation
    // ============================================================================

    void GetConfigPropertyHandler::handleRequest(Poco::Net::HTTPServerRequest &request,
                                                 Poco::Net::HTTPServerResponse &response)
    {
        if (request.getMethod() != "GET")
        {
            sendErrorResponse(response, "Method not allowed", 405);
            return;
        }

        try
        {
            std::string key = extractKeyFromPath(request.getURI());
            if (key.empty())
            {
                sendErrorResponse(response, "Invalid property key", 400);
                return;
            }

            auto property = config_manager_->getProperty<std::any>(key);
            if (!property)
            {
                sendErrorResponse(response, "Property not found: " + key, 404);
                return;
            }

            Poco::JSON::Object property_obj;
            property_obj.set("key", key);
            property_obj.set("value", property->getValueAsString());
            property_obj.set("description", property->getDescription());
            property_obj.set("modified", property->isModified());

            std::stringstream ss;
            property_obj.stringify(ss);
            sendJsonResponse(response, ss.str());
        }
        catch (const std::exception &e)
        {
            sendErrorResponse(response, "Failed to retrieve property: " + std::string(e.what()), 500);
        }
    }

    // ============================================================================
    // UpdateConfigPropertyHandler Implementation
    // ============================================================================

    void UpdateConfigPropertyHandler::handleRequest(Poco::Net::HTTPServerRequest &request,
                                                    Poco::Net::HTTPServerResponse &response)
    {
        if (request.getMethod() != "PUT")
        {
            sendErrorResponse(response, "Method not allowed", 405);
            return;
        }

        try
        {
            std::string key = extractKeyFromPath(request.getURI());
            if (key.empty())
            {
                sendErrorResponse(response, "Invalid property key", 400);
                return;
            }

            std::string body = getRequestBody(request);
            if (body.empty())
            {
                sendErrorResponse(response, "Request body is required", 400);
                return;
            }

            // Parse JSON request body
            Poco::JSON::Parser parser;
            auto json_obj = parser.parse(body).extract<Poco::JSON::Object::Ptr>();

            if (!json_obj->has("value"))
            {
                sendErrorResponse(response, "Value field is required", 400);
                return;
            }

            std::string value_str = json_obj->get("value").toString();

            // Try to update the property
            auto property = config_manager_->getProperty<std::any>(key);
            if (!property)
            {
                sendErrorResponse(response, "Property not found: " + key, 404);
                return;
            }

            // Update property value
            if (property->setValueFromString(value_str))
            {
                // Trigger save to persist changes
                config_manager_->triggerSave();

                Poco::JSON::Object success_obj;
                success_obj.set("message", "Property updated successfully");
                success_obj.set("key", key);
                success_obj.set("value", value_str);

                std::stringstream ss;
                success_obj.stringify(ss);
                sendJsonResponse(response, ss.str());
            }
            else
            {
                sendErrorResponse(response, "Failed to update property: " + key, 400);
            }
        }
        catch (const std::exception &e)
        {
            sendErrorResponse(response, "Failed to update property: " + std::string(e.what()), 500);
        }
    }

    // ============================================================================
    // ReloadConfigHandler Implementation
    // ============================================================================

    void ReloadConfigHandler::handleRequest(Poco::Net::HTTPServerRequest &request,
                                            Poco::Net::HTTPServerResponse &response)
    {
        if (request.getMethod() != "POST")
        {
            sendErrorResponse(response, "Method not allowed", 405);
            return;
        }

        try
        {
            if (config_manager_->reloadConfiguration())
            {
                Poco::JSON::Object success_obj;
                success_obj.set("message", "Configuration reloaded successfully");

                std::stringstream ss;
                success_obj.stringify(ss);
                sendJsonResponse(response, ss.str());
            }
            else
            {
                sendErrorResponse(response, "Failed to reload configuration", 500);
            }
        }
        catch (const std::exception &e)
        {
            sendErrorResponse(response, "Failed to reload configuration: " + std::string(e.what()), 500);
        }
    }

    // ============================================================================
    // ConfigStatusHandler Implementation
    // ============================================================================

    void ConfigStatusHandler::handleRequest(Poco::Net::HTTPServerRequest &request,
                                            Poco::Net::HTTPServerResponse &response)
    {
        if (request.getMethod() != "GET")
        {
            sendErrorResponse(response, "Method not allowed", 405);
            return;
        }

        try
        {
            Poco::JSON::Object status_obj;
            status_obj.set("valid", config_manager_->isValid());
            status_obj.set("config_file", config_manager_->getConfigFilePath());
            status_obj.set("property_count", config_manager_->getAllPropertyKeys().size());

            // Get validation errors
            auto errors = config_manager_->getValidationErrors();
            Poco::JSON::Array errors_array;
            for (const auto &error : errors)
            {
                errors_array.add(error);
            }
            status_obj.set("validation_errors", errors_array);

            std::stringstream ss;
            status_obj.stringify(ss);
            sendJsonResponse(response, ss.str());
        }
        catch (const std::exception &e)
        {
            sendErrorResponse(response, "Failed to get status: " + std::string(e.what()), 500);
        }
    }

    // ============================================================================
    // OpenApiSpecHandler Implementation
    // ============================================================================

    void OpenApiSpecHandler::handleRequest(Poco::Net::HTTPServerRequest &request,
                                           Poco::Net::HTTPServerResponse &response)
    {
        if (request.getMethod() != "GET")
        {
            sendErrorResponse(response, "Method not allowed", 405);
            return;
        }

        try
        {
            Poco::JSON::Object openapi_spec;
            openapi_spec.set("openapi", "3.0.0");

            // Create info object
            Poco::JSON::Object info;
            info.set("title", "Media Deduplication Server Configuration API");
            info.set("version", "1.0.0");
            info.set("description", "RESTful API for managing server configuration");
            openapi_spec.set("info", info);

            // Define paths
            Poco::JSON::Object paths;

            // GET /api/v1/config
            Poco::JSON::Object get_all_config;
            get_all_config.set("summary", "Get all configuration properties");

            Poco::JSON::Object responses_200;
            responses_200.set("description", "Configuration retrieved successfully");

            Poco::JSON::Object content;
            Poco::JSON::Object schema;
            schema.set("type", "object");
            schema.set("additionalProperties", true);
            content.set("schema", schema);

            Poco::JSON::Object app_json;
            app_json.set("application/json", content);
            responses_200.set("content", app_json);

            Poco::JSON::Object responses;
            responses.set("200", responses_200);
            get_all_config.set("responses", responses);

            Poco::JSON::Object get_method;
            get_method.set("get", get_all_config);
            paths.set("/api/v1/config", get_method);

            // GET /api/v1/config/{key}
            Poco::JSON::Object get_property;
            get_property.set("summary", "Get specific configuration property");

            Poco::JSON::Array parameters;
            Poco::JSON::Object param;
            param.set("name", "key");
            param.set("in", "path");
            param.set("required", true);

            Poco::JSON::Object param_schema;
            param_schema.set("type", "string");
            param.set("schema", param_schema);

            parameters.add(param);
            get_property.set("parameters", parameters);

            Poco::JSON::Object get_property_responses;
            Poco::JSON::Object get_200;
            get_200.set("description", "Property retrieved successfully");
            get_property_responses.set("200", get_200);

            Poco::JSON::Object get_404;
            get_404.set("description", "Property not found");
            get_property_responses.set("404", get_404);

            get_property.set("responses", get_property_responses);

            Poco::JSON::Object get_property_method;
            get_property_method.set("get", get_property);
            paths.set("/api/v1/config/{key}", get_property_method);

            // PUT /api/v1/config/{key}
            Poco::JSON::Object put_property;
            put_property.set("summary", "Update configuration property");

            Poco::JSON::Array put_parameters;
            Poco::JSON::Object put_param;
            put_param.set("name", "key");
            put_param.set("in", "path");
            put_param.set("required", true);

            Poco::JSON::Object put_param_schema;
            put_param_schema.set("type", "string");
            put_param.set("schema", put_param_schema);

            put_parameters.add(put_param);
            put_property.set("parameters", put_parameters);

            // Request body
            Poco::JSON::Object request_body;
            request_body.set("required", true);

            Poco::JSON::Object put_content;
            Poco::JSON::Object put_schema;
            put_schema.set("type", "object");

            Poco::JSON::Object properties;
            Poco::JSON::Object value_prop;
            value_prop.set("type", "string");
            properties.set("value", value_prop);
            put_schema.set("properties", properties);

            Poco::JSON::Array required;
            required.add("value");
            put_schema.set("required", required);

            put_content.set("schema", put_schema);
            Poco::JSON::Object app_json_put;
            app_json_put.set("application/json", put_content);
            request_body.set("content", app_json_put);

            put_property.set("requestBody", request_body);

            // Responses
            Poco::JSON::Object put_responses;
            Poco::JSON::Object put_200;
            put_200.set("description", "Property updated successfully");
            put_responses.set("200", put_200);

            Poco::JSON::Object put_400;
            put_400.set("description", "Invalid request");
            put_responses.set("400", put_400);

            Poco::JSON::Object put_404;
            put_404.set("description", "Property not found");
            put_responses.set("404", put_404);

            put_property.set("responses", put_responses);

            Poco::JSON::Object put_method;
            put_method.set("put", put_property);
            paths.set("/api/v1/config/{key}", put_method);

            // POST /api/v1/config/reload
            Poco::JSON::Object reload;
            reload.set("summary", "Reload configuration from file");

            Poco::JSON::Object reload_responses;
            Poco::JSON::Object reload_200;
            reload_200.set("description", "Configuration reloaded successfully");
            reload_responses.set("200", reload_200);

            Poco::JSON::Object reload_500;
            reload_500.set("description", "Failed to reload configuration");
            reload_responses.set("500", reload_500);

            reload.set("responses", reload_responses);

            Poco::JSON::Object post_method;
            post_method.set("post", reload);
            paths.set("/api/v1/config/reload", post_method);

            // GET /api/v1/config/status
            Poco::JSON::Object status;
            status.set("summary", "Get configuration system status");

            Poco::JSON::Object status_responses;
            Poco::JSON::Object status_200;
            status_200.set("description", "Status retrieved successfully");
            status_responses.set("200", status_200);

            status.set("responses", status_responses);

            Poco::JSON::Object status_method;
            status_method.set("get", status);
            paths.set("/api/v1/config/status", status_method);

            // User settings paths
            {
                // GET /api/v1/user-settings
                Poco::JSON::Object list_op;
                list_op.set("summary", "List all user settings");
                Poco::JSON::Object list_responses;
                Poco::JSON::Object list_200;
                list_200.set("description", "OK");
                list_responses.set("200", list_200);
                list_op.set("responses", list_responses);
                Poco::JSON::Object list_path;
                list_path.set("get", list_op);
                paths.set("/api/v1/user-settings", list_path);

                // GET/PUT/DELETE /api/v1/user-settings/{key}
                Poco::JSON::Array key_params;
                Poco::JSON::Object key_param;
                key_param.set("name", "key");
                key_param.set("in", "path");
                key_param.set("required", true);
                Poco::JSON::Object key_schema;
                key_schema.set("type", "string");
                key_param.set("schema", key_schema);
                key_params.add(key_param);

                Poco::JSON::Object get_op;
                get_op.set("summary", "Get a user setting");
                get_op.set("parameters", key_params);
                Poco::JSON::Object get_responses;
                Poco::JSON::Object get_200;
                get_200.set("description", "OK");
                Poco::JSON::Object get_404;
                get_404.set("description", "Not Found");
                get_responses.set("200", get_200);
                get_responses.set("404", get_404);
                get_op.set("responses", get_responses);

                Poco::JSON::Object put_op;
                put_op.set("summary", "Create or update a user setting");
                put_op.set("parameters", key_params);
                Poco::JSON::Object put_body;
                put_body.set("required", true);
                Poco::JSON::Object put_ct;
                Poco::JSON::Object put_schema;
                put_schema.set("type", "object");
                Poco::JSON::Object put_props;
                Poco::JSON::Object put_val;
                put_val.set("type", "string");
                put_props.set("value", put_val);
                put_schema.set("properties", put_props);
                Poco::JSON::Array put_req;
                put_req.add("value");
                put_schema.set("required", put_req);
                put_ct.set("schema", put_schema);
                Poco::JSON::Object put_ctype;
                put_ctype.set("application/json", put_ct);
                put_body.set("content", put_ctype);
                put_op.set("requestBody", put_body);
                Poco::JSON::Object put_responses;
                Poco::JSON::Object put_200;
                put_200.set("description", "OK");
                put_responses.set("200", put_200);
                put_op.set("responses", put_responses);

                Poco::JSON::Object del_op;
                del_op.set("summary", "Delete a user setting");
                del_op.set("parameters", key_params);
                Poco::JSON::Object del_responses;
                Poco::JSON::Object del_200;
                del_200.set("description", "OK");
                Poco::JSON::Object del_404;
                del_404.set("description", "Not Found");
                del_responses.set("200", del_200);
                del_responses.set("404", del_404);
                del_op.set("responses", del_responses);

                Poco::JSON::Object key_path;
                key_path.set("get", get_op);
                key_path.set("put", put_op);
                key_path.set("delete", del_op);
                paths.set("/api/v1/user-settings/{key}", key_path);
            }

            openapi_spec.set("paths", paths);

            std::stringstream ss;
            openapi_spec.stringify(ss);
            sendJsonResponse(response, ss.str());
        }
        catch (const std::exception &e)
        {
            sendErrorResponse(response, "Failed to generate OpenAPI spec: " + std::string(e.what()), 500);
        }
    }
    // -------------------- User Settings Handlers --------------------

    ListUserSettingsHandler::ListUserSettingsHandler(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                                                     std::shared_ptr<UserSettingsService> service)
        : ConfigRequestHandler(config_manager), service_(std::move(service)) {}

    void ListUserSettingsHandler::handleRequest(Poco::Net::HTTPServerRequest &request,
                                                Poco::Net::HTTPServerResponse &response)
    {
        if (request.getMethod() != "GET")
        {
            sendErrorResponse(response, "Method not allowed", 405);
            return;
        }
        if (!service_)
        {
            sendErrorResponse(response, "Service unavailable", 503);
            return;
        }
        auto map = service_->listSettings();
        Poco::JSON::Object obj;
        for (const auto &kv : map)
            obj.set(kv.first, kv.second);
        std::ostringstream oss;
        obj.stringify(oss);
        sendJsonResponse(response, oss.str());
    }

    static std::string extractUserSettingsKey(const std::string &path)
    {
        std::regex path_regex(R"(/api/v1/user-settings/(.+))");
        std::smatch match;
        if (std::regex_match(path, match, path_regex) && match.size() > 1)
            return match[1].str();
        return {};
    }

    GetUserSettingHandler::GetUserSettingHandler(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                                                 std::shared_ptr<UserSettingsService> service)
        : ConfigRequestHandler(config_manager), service_(std::move(service)) {}

    void GetUserSettingHandler::handleRequest(Poco::Net::HTTPServerRequest &request,
                                              Poco::Net::HTTPServerResponse &response)
    {
        if (request.getMethod() != "GET")
        {
            sendErrorResponse(response, "Method not allowed", 405);
            return;
        }
        if (!service_)
        {
            sendErrorResponse(response, "Service unavailable", 503);
            return;
        }
        std::string key = extractUserSettingsKey(request.getURI());
        if (key.empty())
        {
            sendErrorResponse(response, "Invalid key", 400);
            return;
        }
        std::string value;
        if (!service_->getSetting(key, value))
        {
            sendErrorResponse(response, "Not found", 404);
            return;
        }
        Poco::JSON::Object obj;
        obj.set("key", key);
        obj.set("value", value);
        std::ostringstream oss;
        obj.stringify(oss);
        sendJsonResponse(response, oss.str());
    }

    PutUserSettingHandler::PutUserSettingHandler(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                                                 std::shared_ptr<UserSettingsService> service)
        : ConfigRequestHandler(config_manager), service_(std::move(service)) {}

    void PutUserSettingHandler::handleRequest(Poco::Net::HTTPServerRequest &request,
                                              Poco::Net::HTTPServerResponse &response)
    {
        if (request.getMethod() != "PUT")
        {
            sendErrorResponse(response, "Method not allowed", 405);
            return;
        }
        if (!service_)
        {
            sendErrorResponse(response, "Service unavailable", 503);
            return;
        }
        std::string key = extractUserSettingsKey(request.getURI());
        if (key.empty())
        {
            sendErrorResponse(response, "Invalid key", 400);
            return;
        }
        std::string body = getRequestBody(request);
        if (body.empty())
        {
            sendErrorResponse(response, "Body required", 400);
            return;
        }
        Poco::JSON::Parser parser;
        auto json = parser.parse(body).extract<Poco::JSON::Object::Ptr>();
        if (!json->has("value"))
        {
            sendErrorResponse(response, "Missing value", 400);
            return;
        }
        std::string value = json->get("value").toString();
        if (!service_->upsertSetting(key, value))
        {
            sendErrorResponse(response, "Failed to save", 500);
            return;
        }
        Poco::JSON::Object obj;
        obj.set("status", "ok");
        obj.set("key", key);
        obj.set("value", value);
        std::ostringstream oss;
        obj.stringify(oss);
        sendJsonResponse(response, oss.str());
    }

    DeleteUserSettingHandler::DeleteUserSettingHandler(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                                                       std::shared_ptr<UserSettingsService> service)
        : ConfigRequestHandler(config_manager), service_(std::move(service)) {}

    void DeleteUserSettingHandler::handleRequest(Poco::Net::HTTPServerRequest &request,
                                                 Poco::Net::HTTPServerResponse &response)
    {
        if (request.getMethod() != "DELETE")
        {
            sendErrorResponse(response, "Method not allowed", 405);
            return;
        }
        if (!service_)
        {
            sendErrorResponse(response, "Service unavailable", 503);
            return;
        }
        std::string key = extractUserSettingsKey(request.getURI());
        if (key.empty())
        {
            sendErrorResponse(response, "Invalid key", 400);
            return;
        }
        if (!service_->deleteSetting(key))
        {
            sendErrorResponse(response, "Not found", 404);
            return;
        }
        Poco::JSON::Object obj;
        obj.set("status", "ok");
        obj.set("key", key);
        std::ostringstream oss;
        obj.stringify(oss);
        sendJsonResponse(response, oss.str());
    }

    // ============================================================================
    // RestartWebServerHandler Implementation
    // ============================================================================

    RestartWebServerHandler::RestartWebServerHandler(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                                                     std::shared_ptr<WebServer> web_server)
        : ConfigRequestHandler(config_manager), web_server_(web_server)
    {
    }

    void RestartWebServerHandler::handleRequest(Poco::Net::HTTPServerRequest &request,
                                                Poco::Net::HTTPServerResponse &response)
    {
        try
        {
            if (!web_server_)
            {
                sendErrorResponse(response, "Web server not available", 503);
                return;
            }

            // Check if web server is running
            if (!web_server_->isRunning())
            {
                sendErrorResponse(response, "Web server is not running", 503);
                return;
            }

            // Restart web server with new configuration
            bool success = web_server_->restartWithNewConfig();

            if (success)
            {
                Poco::JSON::Object result;
                result.set("status", "success");
                result.set("message", "Web server restarted successfully");
                result.set("host", web_server_->getHost());
                result.set("port", static_cast<int>(web_server_->getPort()));

                std::ostringstream oss;
                result.stringify(oss);
                sendJsonResponse(response, oss.str());
            }
            else
            {
                sendErrorResponse(response, "Failed to restart web server", 500);
            }
        }
        catch (const std::exception &e)
        {
            sendErrorResponse(response, "Error restarting web server: " + std::string(e.what()), 500);
        }
    }

} // namespace MediaDedup
