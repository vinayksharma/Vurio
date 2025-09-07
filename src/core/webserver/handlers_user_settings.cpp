#include "core/web_server.hpp"
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <sstream>
#include <regex>
#include "database/user_settings_service.hpp"
#include "filesmanager/files_service.hpp"

namespace MediaDedup
{

    ListUserSettingsHandler::ListUserSettingsHandler(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                                                     std::shared_ptr<UserSettingsService> service)
        : ConfigRequestHandler(std::move(config_manager)), service_(std::move(service)) {}

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

    static std::string extractUserSettingsKey2(const std::string &path)
    {
        std::regex path_regex(R"(/api/v1/user-settings/(.+))");
        std::smatch match;
        if (std::regex_match(path, match, path_regex) && match.size() > 1)
            return match[1].str();
        return {};
    }

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
        std::string key = extractUserSettingsKey2(request.getURI());
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

    GetUserSettingHandler::GetUserSettingHandler(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                                                 std::shared_ptr<UserSettingsService> service)
        : ConfigRequestHandler(std::move(config_manager)), service_(std::move(service)) {}

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
        std::string key = extractUserSettingsKey2(request.getURI());
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

    PutUserSettingHandler::PutUserSettingHandler(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                                                 std::shared_ptr<UserSettingsService> service)
        : ConfigRequestHandler(std::move(config_manager)), service_(std::move(service)) {}

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
        std::string key = extractUserSettingsKey2(request.getURI());
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

    DeleteUserSettingHandler::DeleteUserSettingHandler(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                                                       std::shared_ptr<UserSettingsService> service)
        : ConfigRequestHandler(std::move(config_manager)), service_(std::move(service)) {}

    // -------- Media Location endpoints (register/deregister) --------

    RegisterMediaLocationHandler::RegisterMediaLocationHandler(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                                                               std::shared_ptr<FilesService> service)
        : ConfigRequestHandler(std::move(config_manager)), service_(std::move(service)) {}
    void RegisterMediaLocationHandler::handleRequest(Poco::Net::HTTPServerRequest &request,
                                                     Poco::Net::HTTPServerResponse &response)
    {
        if (request.getMethod() != "POST")
        {
            sendErrorResponse(response, "Method not allowed", 405);
            return;
        }
        if (!service_)
        {
            sendErrorResponse(response, "Service unavailable", 503);
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
        if (!json->has("directory"))
        {
            sendErrorResponse(response, "Missing directory", 400);
            return;
        }
        std::string dir = json->get("directory").toString();
        if (dir.empty())
        {
            sendErrorResponse(response, "Invalid directory", 400);
            return;
        }
        if (!service_->registerMediaLocation(dir))
        {
            sendErrorResponse(response, "Failed to register", 500);
            return;
        }
        Poco::JSON::Object obj;
        obj.set("status", "ok");
        obj.set("directory", dir);
        std::ostringstream oss;
        obj.stringify(oss);
        sendJsonResponse(response, oss.str());
    }

    DeregisterMediaLocationHandler::DeregisterMediaLocationHandler(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                                                                   std::shared_ptr<FilesService> service)
        : ConfigRequestHandler(std::move(config_manager)), service_(std::move(service)) {}
    void DeregisterMediaLocationHandler::handleRequest(Poco::Net::HTTPServerRequest &request,
                                                       Poco::Net::HTTPServerResponse &response)
    {
        if (request.getMethod() != "POST")
        {
            sendErrorResponse(response, "Method not allowed", 405);
            return;
        }
        if (!service_)
        {
            sendErrorResponse(response, "Service unavailable", 503);
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
        if (!json->has("directory"))
        {
            sendErrorResponse(response, "Missing directory", 400);
            return;
        }
        std::string dir = json->get("directory").toString();
        if (dir.empty())
        {
            sendErrorResponse(response, "Invalid directory", 400);
            return;
        }
        if (!service_->deregisterMediaLocation(dir))
        {
            sendErrorResponse(response, "Failed to deregister", 500);
            return;
        }
        Poco::JSON::Object obj;
        obj.set("status", "ok");
        obj.set("directory", dir);
        std::ostringstream oss;
        obj.stringify(oss);
        sendJsonResponse(response, oss.str());
    }

} // namespace MediaDedup
