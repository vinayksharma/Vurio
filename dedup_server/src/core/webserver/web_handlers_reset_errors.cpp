#include "core/web/web_handlers_reset_errors.hpp"
#include "services/eject_service.hpp"
#include "config/config_enums.hpp"
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <Poco/JSON/Array.h>
#include <Poco/UUIDGenerator.h>
#include <Poco/SHA1Engine.h>
#include <Poco/DigestEngine.h>
#include <Poco/DigestStream.h>
#include <Poco/FileStream.h>
#include <Poco/StreamCopier.h>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include "database/database_manager.hpp"
#include "database/scanned_files_ops.hpp"
#include "database/user_settings_ops.hpp"
#include "database/ejected_files_ops.hpp"

namespace MediaDedup
{

    // Helper functions
    static void sendJsonResponse(Poco::Net::HTTPServerResponse &response, const std::string &json_data, int status_code = 200)
    {
        response.setStatus(static_cast<Poco::Net::HTTPResponse::HTTPStatus>(status_code));
        response.setContentType("application/json; charset=utf-8");
        response.setContentLength(json_data.length());
        response.send() << json_data;
    }

    static void sendErrorResponse(Poco::Net::HTTPServerResponse &response, const std::string &error_message, int status_code = 400)
    {
        Poco::JSON::Object error_obj;
        error_obj.set("error", error_message);
        std::stringstream ss;
        error_obj.stringify(ss);
        sendJsonResponse(response, ss.str(), status_code);
    }

    ResetErrorsHandler::ResetErrorsHandler(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                                           std::shared_ptr<ScannedFilesService> scanned_files_service)
        : config_manager_(std::move(config_manager)),
          scanned_files_service_(std::move(scanned_files_service))
    {
    }

    void ResetErrorsHandler::handleRequest(Poco::Net::HTTPServerRequest &request,
                                           Poco::Net::HTTPServerResponse &response)
    {
        if (request.getMethod() != "POST")
        {
            sendErrorResponse(response, "Method not allowed", 405);
            return;
        }

        try
        {
            // Reset all errors
            int files_reset = scanned_files_service_->resetAllErrors();

            if (files_reset < 0)
            {
                sendErrorResponse(response, "Failed to reset errors", 500);
                return;
            }

            // Create success response
            Poco::JSON::Object response_obj;
            response_obj.set("success", true);

            std::stringstream ss;
            response_obj.stringify(ss);
            sendJsonResponse(response, ss.str());
        }
        catch (const std::exception &e)
        {
            sendErrorResponse(response, "Internal server error: " + std::string(e.what()), 500);
        }
    }

    // AR-2: the eject/undo logic lives in Services::EjectService (the HTTP
    // fork was the correct one and moved there verbatim); these handlers
    // marshal JSON only.

    namespace
    {
        void sendBulkResult(Poco::Net::HTTPServerResponse &response,
                            const MediaDedup::Services::BulkOpResult &r)
        {
            Poco::JSON::Object result_obj;
            result_obj.set("success", r.successCount > 0 || r.failureCount == 0);
            result_obj.set("success_count", r.successCount);
            result_obj.set("failure_count", r.failureCount);
            if (r.successCount == 0 && !r.lastError.empty()) {
                result_obj.set("error", r.lastError);
            }
            std::ostringstream oss;
            result_obj.stringify(oss);
            sendJsonResponse(response, oss.str());
        }
    }

    EjectErrorsHandler::EjectErrorsHandler(std::shared_ptr<DatabaseManager> db)
        : database_manager_(std::move(db))
    {
    }

    void EjectErrorsHandler::handleRequest(Poco::Net::HTTPServerRequest &request,
                                           Poco::Net::HTTPServerResponse &response)
    {
        if (request.getMethod() != "POST")
        {
            sendErrorResponse(response, "Method not allowed", 405);
            return;
        }
        try
        {
            sendBulkResult(response, Services::ejectErrored(*database_manager_));
        }
        catch (const std::exception &e)
        {
            sendErrorResponse(response, "Internal server error: " + std::string(e.what()), 500);
        }
    }

    UndoEjectErrorsHandler::UndoEjectErrorsHandler(std::shared_ptr<DatabaseManager> db)
        : database_manager_(std::move(db))
    {
    }

    void UndoEjectErrorsHandler::handleRequest(Poco::Net::HTTPServerRequest &request,
                                               Poco::Net::HTTPServerResponse &response)
    {
        if (request.getMethod() != "POST")
        {
            sendErrorResponse(response, "Method not allowed", 405);
            return;
        }
        try
        {
            sendBulkResult(response, Services::undoEjectErrored(*database_manager_));
        }
        catch (const std::exception &e)
        {
            sendErrorResponse(response, "Internal server error: " + std::string(e.what()), 500);
        }
    }

    EjectIgnoredHandler::EjectIgnoredHandler(std::shared_ptr<DatabaseManager> db)
        : database_manager_(std::move(db))
    {
    }

    void EjectIgnoredHandler::handleRequest(Poco::Net::HTTPServerRequest &request,
                                           Poco::Net::HTTPServerResponse &response)
    {
        if (request.getMethod() != "POST")
        {
            sendErrorResponse(response, "Method not allowed", 405);
            return;
        }
        try
        {
            sendBulkResult(response, Services::ejectIgnored(*database_manager_));
        }
        catch (const std::exception &e)
        {
            sendErrorResponse(response, "Internal server error: " + std::string(e.what()), 500);
        }
    }

    UndoEjectIgnoredHandler::UndoEjectIgnoredHandler(std::shared_ptr<DatabaseManager> db)
        : database_manager_(std::move(db))
    {
    }

    void UndoEjectIgnoredHandler::handleRequest(Poco::Net::HTTPServerRequest &request,
                                               Poco::Net::HTTPServerResponse &response)
    {
        if (request.getMethod() != "POST")
        {
            sendErrorResponse(response, "Method not allowed", 405);
            return;
        }
        try
        {
            sendBulkResult(response, Services::undoEjectIgnored(*database_manager_));
        }
        catch (const std::exception &e)
        {
            sendErrorResponse(response, "Internal server error: " + std::string(e.what()), 500);
        }
    }

} // namespace MediaDedup
