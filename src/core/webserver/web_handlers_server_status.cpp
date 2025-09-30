#include "core/web/web_handlers_server_status.hpp"
#include "config/config_enums.hpp"
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Array.h>
#include <sstream>
#include <chrono>

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

    ServerStatusHandler::ServerStatusHandler(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                                             std::shared_ptr<FilesService> files_service,
                                             std::shared_ptr<ScannedFilesService> scanned_files_service,
                                             std::shared_ptr<ThreadPoolManager> tpm)
        : config_manager_(std::move(config_manager)),
          files_service_(std::move(files_service)),
          scanned_files_service_(std::move(scanned_files_service)),
          tpm_(std::move(tpm))
    {
    }

    void ServerStatusHandler::handleRequest(Poco::Net::HTTPServerRequest &request,
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

            // Server basic info
            status_obj.set("server_name", "Media Deduplication Server");
            status_obj.set("status", "running");

            // Configuration status
            if (config_manager_)
            {
                Poco::JSON::Object config_status;
                config_status.set("valid", config_manager_->isValid());
                config_status.set("config_file", config_manager_->getConfigFilePath());
                config_status.set("property_count", static_cast<int>(config_manager_->getAllPropertyKeys().size()));

                auto errors = config_manager_->getValidationErrors();
                Poco::JSON::Array errors_array;
                for (const auto &error : errors)
                    errors_array.add(error);
                config_status.set("validation_errors", errors_array);

                status_obj.set("configuration", config_status);

                // Server mode
                auto server_mode = config_manager_->getServerMode("server.mode", ServerMode::FAST);
                status_obj.set("server_mode", toString(server_mode));
            }

            // Scanned files count
            if (scanned_files_service_)
            {
                int scanned_count = scanned_files_service_->count();
                status_obj.set("scanned_files_count", scanned_count);

                // Get processed count for current server mode
                if (config_manager_)
                {
                    auto server_mode = config_manager_->getServerMode("server.mode", ServerMode::FAST);
                    int processed_count = scanned_files_service_->countProcessed(server_mode);
                    status_obj.set("processed_files_count", processed_count);
                }
                else
                {
                    // Fallback to total processed count if config manager not available
                    int processed_count = scanned_files_service_->countProcessed();
                    status_obj.set("processed_files_count", processed_count);
                }
            }

            // Registered directories
            if (files_service_)
            {
                auto locations = files_service_->listMediaLocations();
                Poco::JSON::Array directories_array;

                for (const auto &kv : locations)
                {
                    directories_array.add(kv.second); // kv.second is the original path
                }

                status_obj.set("registered_directories", directories_array);
                status_obj.set("registered_directories_count", static_cast<int>(locations.size()));
            }

            // Thread pool manager status
            if (tpm_)
            {
                auto tpm_status = tpm_->getStatus();
                Poco::JSON::Object tpm_obj;
                tpm_obj.set("effective_max_threads", static_cast<int>(tpm_status.effectiveMax));
                tpm_obj.set("running_total", static_cast<int>(tpm_status.runningTotal));

                Poco::JSON::Object::Ptr per_type_obj = new Poco::JSON::Object();
                for (const auto &kv : tpm_status.perType)
                {
                    Poco::JSON::Object::Ptr type_obj = new Poco::JSON::Object();
                    type_obj->set("share", kv.second.share);
                    type_obj->set("running", static_cast<int>(kv.second.running));
                    type_obj->set("queued", static_cast<int>(kv.second.queued));
                    per_type_obj->set(kv.first, type_obj);
                }
                tpm_obj.set("per_type", per_type_obj);

                status_obj.set("thread_pool", tpm_obj);
            }

            // Timestamp
            auto now = std::chrono::system_clock::now();
            auto time_t = std::chrono::system_clock::to_time_t(now);
            status_obj.set("timestamp", static_cast<long>(time_t));

            std::stringstream ss;
            status_obj.stringify(ss);
            sendJsonResponse(response, ss.str());
        }
        catch (const std::exception &e)
        {
            sendErrorResponse(response, "Failed to get server status: " + std::string(e.what()), 500);
        }
    }

} // namespace MediaDedup
