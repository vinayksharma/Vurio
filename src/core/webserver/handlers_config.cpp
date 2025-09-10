#include "core/web_server.hpp"
#include "config/unified_observable_config.hpp"
#include <Poco/StreamCopier.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Parser.h>
#include "orchestration/thread_pool_manager.hpp"
#include <regex>
#include <sstream>

namespace MediaDedup
{
    // OpenApiSpecHandler vtable anchor via method definition already in this TU

    // Base handler ctor
    ConfigRequestHandler::ConfigRequestHandler(std::shared_ptr<UnifiedObservableConfigManager> config_manager)
        : config_manager_(std::move(config_manager)) {}

    // RestartWebServerHandler ctor
    RestartWebServerHandler::RestartWebServerHandler(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                                                     std::shared_ptr<WebServer> web_server)
        : ConfigRequestHandler(std::move(config_manager)), web_server_(std::move(web_server)) {}

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
            if (!web_server_->isRunning())
            {
                sendErrorResponse(response, "Web server is not running", 503);
                return;
            }
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
            sendErrorResponse(response, std::string("Error restarting web server: ") + e.what(), 500);
        }
    }

    void ConfigRequestHandler::handleRequest(Poco::Net::HTTPServerRequest &request,
                                             Poco::Net::HTTPServerResponse &response)
    {
        response.set("Access-Control-Allow-Origin", "*");
        response.set("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        response.set("Access-Control-Allow-Headers", "Content-Type, Authorization");
        if (request.getMethod() == "OPTIONS")
        {
            response.setStatusAndReason(Poco::Net::HTTPResponse::HTTP_OK);
            response.send();
            return;
        }
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
        std::regex path_regex(R"(/api/v1/config/(.+))");
        std::smatch match;
        if (std::regex_match(path, match, path_regex) && match.size() > 1)
            return match[1].str();
        return "";
    }

    // GetAllConfigHandler
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
            auto keys = config_manager_->getAllPropertyKeys();
            for (const auto &key : keys)
            {
                auto property = config_manager_->getProperty<std::any>(key);
                if (!property)
                    continue;
                auto value = property->getValue();
                if (value.type() == typeid(std::string))
                    config_obj.set(key, std::any_cast<std::string>(value));
                else if (value.type() == typeid(int))
                    config_obj.set(key, std::any_cast<int>(value));
                else if (value.type() == typeid(double))
                    config_obj.set(key, std::any_cast<double>(value));
                else if (value.type() == typeid(bool))
                    config_obj.set(key, std::any_cast<bool>(value));
                else
                    config_obj.set(key, property->getValueAsString());
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

    // GetConfigPropertyHandler
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

    // UpdateConfigPropertyHandler
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
            Poco::JSON::Parser parser;
            auto json_obj = parser.parse(body).extract<Poco::JSON::Object::Ptr>();
            if (!json_obj->has("value"))
            {
                sendErrorResponse(response, "Value field is required", 400);
                return;
            }
            std::string value_str = json_obj->get("value").toString();
            auto property = config_manager_->getProperty<std::any>(key);
            if (!property)
            {
                sendErrorResponse(response, "Property not found: " + key, 404);
                return;
            }
            if (property->setValueFromString(value_str))
            {
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

    // ReloadConfigHandler
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

    // ConfigStatusHandler
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
            auto errors = config_manager_->getValidationErrors();
            Poco::JSON::Array errors_array;
            for (const auto &error : errors)
                errors_array.add(error);
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

    // TPMStatusHandler implementation
    TPMStatusHandler::TPMStatusHandler(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                                       std::shared_ptr<ThreadPoolManager> tpm)
        : ConfigRequestHandler(std::move(config_manager)), tpm_(std::move(tpm)) {}

    void TPMStatusHandler::handleRequest(Poco::Net::HTTPServerRequest &request,
                                         Poco::Net::HTTPServerResponse &response)
    {
        if (request.getMethod() != "GET")
        {
            sendErrorResponse(response, "Method not allowed", 405);
            return;
        }
        Poco::JSON::Object::Ptr root = new Poco::JSON::Object();
        if (!tpm_)
        {
            root->set("status", "unavailable");
            std::ostringstream oss;
            root->stringify(oss);
            sendJsonResponse(response, oss.str(), 503);
            return;
        }
        auto st = tpm_->getStatus();
        root->set("effectiveMax", static_cast<int>(st.effectiveMax));
        root->set("runningTotal", static_cast<int>(st.runningTotal));
        Poco::JSON::Object::Ptr perType = new Poco::JSON::Object();
        for (const auto &kv : st.perType)
        {
            Poco::JSON::Object::Ptr ts = new Poco::JSON::Object();
            ts->set("share", kv.second.share);
            ts->set("running", static_cast<int>(kv.second.running));
            ts->set("queued", static_cast<int>(kv.second.queued));
            perType->set(kv.first, ts);
        }
        root->set("perType", perType);
        std::ostringstream oss;
        root->stringify(oss);
        sendJsonResponse(response, oss.str(), 200);
    }

    // OpenApiSpecHandler
    void OpenApiSpecHandler::handleRequest(Poco::Net::HTTPServerRequest &request,
                                           Poco::Net::HTTPServerResponse &response)
    {
        // Set CORS headers
        response.set("Access-Control-Allow-Origin", "*");
        response.set("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        response.set("Access-Control-Allow-Headers", "Content-Type, Authorization");

        if (request.getMethod() == "OPTIONS")
        {
            response.setStatusAndReason(Poco::Net::HTTPResponse::HTTP_OK);
            response.send();
            return;
        }

        if (request.getMethod() != "GET")
        {
            sendErrorResponse(response, "Method not allowed", 405);
            return;
        }
        try
        {
            Poco::JSON::Object openapi_spec;
            openapi_spec.set("openapi", "3.0.0");
            Poco::JSON::Object info;
            info.set("title", "Media Deduplication Server API");
            info.set("version", "1.0.0");
            info.set("description", "Configuration, User Settings, and TPM API");
            openapi_spec.set("info", info);

            Poco::JSON::Object paths;
            // /api/v1/config
            {
                Poco::JSON::Object op;
                op.set("summary", "Get all configuration properties");
                Poco::JSON::Object res;
                Poco::JSON::Object ok;
                ok.set("description", "OK");
                res.set("200", ok);
                op.set("responses", res);
                Poco::JSON::Object path;
                path.set("get", op);
                paths.set("/api/v1/config", path);
            }

            // /api/v1/config/{key} GET/PUT
            {
                Poco::JSON::Array params;
                Poco::JSON::Object p;
                p.set("name", "key");
                p.set("in", "path");
                p.set("required", true);
                Poco::JSON::Object ps;
                ps.set("type", "string");
                p.set("schema", ps);
                params.add(p);
                Poco::JSON::Object get;
                get.set("summary", "Get specific configuration property");
                get.set("parameters", params);
                Poco::JSON::Object gres;
                Poco::JSON::Object gok;
                gok.set("description", "OK");
                gres.set("200", gok);
                get.set("responses", gres);
                Poco::JSON::Object put;
                put.set("summary", "Update configuration property");
                put.set("parameters", params);
                Poco::JSON::Object body;
                body.set("required", true);
                Poco::JSON::Object ct;
                Poco::JSON::Object schema;
                schema.set("type", "object");
                Poco::JSON::Object props;
                Poco::JSON::Object val;
                val.set("type", "string");
                props.set("value", val);
                schema.set("properties", props);
                ct.set("schema", schema);
                Poco::JSON::Object app;
                app.set("application/json", ct);
                body.set("content", app);
                put.set("requestBody", body);
                Poco::JSON::Object pres;
                Poco::JSON::Object pok;
                pok.set("description", "OK");
                pres.set("200", pok);
                put.set("responses", pres);
                Poco::JSON::Object path;
                path.set("get", get);
                path.set("put", put);
                paths.set("/api/v1/config/{key}", path);
            }

            // /api/v1/config/reload
            {
                Poco::JSON::Object op;
                op.set("summary", "Reload configuration");
                Poco::JSON::Object res;
                Poco::JSON::Object ok;
                ok.set("description", "OK");
                res.set("200", ok);
                op.set("responses", res);
                Poco::JSON::Object path;
                path.set("post", op);
                paths.set("/api/v1/config/reload", path);
            }

            // /api/v1/config/status
            {
                Poco::JSON::Object op;
                op.set("summary", "Get configuration system status");
                Poco::JSON::Object res;
                Poco::JSON::Object ok;
                ok.set("description", "OK");
                res.set("200", ok);
                op.set("responses", res);
                Poco::JSON::Object path;
                path.set("get", op);
                paths.set("/api/v1/config/status", path);
            }

            // User settings
            {
                Poco::JSON::Object list;
                list.set("summary", "List all user settings");
                Poco::JSON::Object res;
                Poco::JSON::Object ok;
                ok.set("description", "OK");
                res.set("200", ok);
                list.set("responses", res);
                Poco::JSON::Object p;
                p.set("get", list);
                paths.set("/api/v1/user-settings", p);
            }
            {
                Poco::JSON::Array params;
                Poco::JSON::Object p;
                p.set("name", "key");
                p.set("in", "path");
                p.set("required", true);
                Poco::JSON::Object ps;
                ps.set("type", "string");
                p.set("schema", ps);
                params.add(p);
                Poco::JSON::Object get;
                get.set("summary", "Get a user setting");
                get.set("parameters", params);
                Poco::JSON::Object gres;
                Poco::JSON::Object gok;
                gok.set("description", "OK");
                Poco::JSON::Object g404;
                g404.set("description", "Not Found");
                gres.set("200", gok);
                gres.set("404", g404);
                get.set("responses", gres);
                Poco::JSON::Object put;
                put.set("summary", "Create or update a user setting");
                put.set("parameters", params);
                Poco::JSON::Object body;
                body.set("required", true);
                Poco::JSON::Object ct;
                Poco::JSON::Object schema;
                schema.set("type", "object");
                Poco::JSON::Object props;
                Poco::JSON::Object val;
                val.set("type", "string");
                props.set("value", val);
                schema.set("properties", props);
                ct.set("schema", schema);
                Poco::JSON::Object app;
                app.set("application/json", ct);
                body.set("content", app);
                put.set("requestBody", body);
                Poco::JSON::Object pres;
                Poco::JSON::Object pok;
                pok.set("description", "OK");
                pres.set("200", pok);
                put.set("responses", pres);
                Poco::JSON::Object del;
                del.set("summary", "Delete a user setting");
                del.set("parameters", params);
                Poco::JSON::Object dres;
                Poco::JSON::Object dok;
                dok.set("description", "OK");
                Poco::JSON::Object d404;
                d404.set("description", "Not Found");
                dres.set("200", dok);
                dres.set("404", d404);
                del.set("responses", dres);
                Poco::JSON::Object path;
                path.set("get", get);
                path.set("put", put);
                path.set("delete", del);
                paths.set("/api/v1/user-settings/{key}", path);
            }

            // Media locations
            {
                Poco::JSON::Object reg;
                reg.set("summary", "Register a media location");
                Poco::JSON::Object body;
                body.set("required", true);
                Poco::JSON::Object ct;
                Poco::JSON::Object schema;
                schema.set("type", "object");
                Poco::JSON::Object props;
                Poco::JSON::Object dir;
                dir.set("type", "string");
                props.set("directory", dir);
                schema.set("properties", props);
                ct.set("schema", schema);
                Poco::JSON::Object app;
                app.set("application/json", ct);
                body.set("content", app);
                reg.set("requestBody", body);
                Poco::JSON::Object res;
                Poco::JSON::Object ok;
                ok.set("description", "OK");
                res.set("200", ok);
                reg.set("responses", res);
                Poco::JSON::Object p;
                p.set("post", reg);
                paths.set("/api/v1/media-locations/register", p);
            }
            {
                Poco::JSON::Object dereg;
                dereg.set("summary", "Deregister a media location");
                Poco::JSON::Object body;
                body.set("required", true);
                Poco::JSON::Object ct;
                Poco::JSON::Object schema;
                schema.set("type", "object");
                Poco::JSON::Object props;
                Poco::JSON::Object dir;
                dir.set("type", "string");
                props.set("directory", dir);
                schema.set("properties", props);
                ct.set("schema", schema);
                Poco::JSON::Object app;
                app.set("application/json", ct);
                body.set("content", app);
                dereg.set("requestBody", body);
                Poco::JSON::Object res;
                Poco::JSON::Object ok;
                ok.set("description", "OK");
                res.set("200", ok);
                dereg.set("responses", res);
                Poco::JSON::Object p;
                p.set("post", dereg);
                paths.set("/api/v1/media-locations/deregister", p);
            }

            // TPM status
            {
                Poco::JSON::Object op;
                op.set("summary", "Get TPM status");
                Poco::JSON::Object res;
                Poco::JSON::Object ok;
                ok.set("description", "OK");
                res.set("200", ok);
                op.set("responses", res);
                Poco::JSON::Object path;
                path.set("get", op);
                paths.set("/api/v1/tpm/status", path);
            }

            openapi_spec.set("paths", paths);
            std::stringstream ss;
            openapi_spec.stringify(ss);
            sendJsonResponse(response, ss.str());
        }
        catch (const std::exception &e)
        {
            sendErrorResponse(response, std::string("Failed to generate OpenAPI spec: ") + e.what(), 500);
        }
    }

} // namespace MediaDedup
