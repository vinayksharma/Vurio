#include "core/web/web_handlers_reset_errors.hpp"
#include "config/config_enums.hpp"
#include <Poco/JSON/Object.h>
#include <sstream>

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

} // namespace MediaDedup
