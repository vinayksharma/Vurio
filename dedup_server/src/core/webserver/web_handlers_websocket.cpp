#include "core/web/web_handlers_websocket.hpp"
#include "config/config_enums.hpp"
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Array.h>
#include <Poco/Net/NetException.h>
#include <Poco/Logger.h>
#include <sstream>
#include <chrono>
#include <thread>

namespace MediaDedup
{

    WebSocketStatusHandler::WebSocketStatusHandler(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                                                   std::shared_ptr<FilesService> files_service,
                                                   std::shared_ptr<ScannedFilesService> scanned_files_service,
                                                   std::shared_ptr<ThreadPoolManager> tpm,
                                                   std::shared_ptr<Orchestration::DuplicateFinder> duplicate_finder)
        : config_manager_(std::move(config_manager)),
          files_service_(std::move(files_service)),
          scanned_files_service_(std::move(scanned_files_service)),
          tpm_(std::move(tpm)),
          duplicate_finder_(std::move(duplicate_finder))
    {
    }

    void WebSocketStatusHandler::handleRequest(Poco::Net::HTTPServerRequest &request,
                                              Poco::Net::HTTPServerResponse &response)
    {
        Poco::Logger &logger = Poco::Logger::get("WebSocketStatusHandler");
        try
        {
            Poco::Net::WebSocket ws(request, response);
            logger.information("WebSocket connection established from %s", request.clientAddress().toString());

            char buffer[1024];
            int flags;
            int n;

            ws.setReceiveTimeout(Poco::Timespan(2, 0)); // 2 seconds timeout for status updates

            do
            {
                // Send current status
                try {
                    std::string json = getStatusJson();
                    ws.sendFrame(json.data(), static_cast<int>(json.size()), Poco::Net::WebSocket::FRAME_TEXT);
                } catch (const std::exception& e) {
                    logger.error("Error sending status over WebSocket: %s", std::string(e.what()));
                    break;
                }

                // Check for incoming messages (like ping or close)
                try {
                    n = ws.receiveFrame(buffer, sizeof(buffer), flags);
                    if (n > 0 && (flags & Poco::Net::WebSocket::FRAME_OP_BITMASK) == Poco::Net::WebSocket::FRAME_OP_CLOSE) {
                        break;
                    }
                } catch (const Poco::TimeoutException&) {
                    // This is expected, we use timeout to loop and send status
                    n = 1; // Keep loop going
                } catch (const Poco::Net::NetException& e) {
                    logger.warning("WebSocket network error: %s", e.displayText());
                    break;
                }
            } while (n > 0);

            logger.information("WebSocket connection closed");
        }
        catch (const Poco::Net::WebSocketException &e)
        {
            logger.error("WebSocket exception: %s", e.displayText());
            switch (e.code())
            {
            case Poco::Net::HTTPResponse::HTTP_BAD_REQUEST:
            case Poco::Net::HTTPResponse::HTTP_UNAUTHORIZED:
            case Poco::Net::HTTPResponse::HTTP_FORBIDDEN:
            case Poco::Net::HTTPResponse::HTTP_NOT_FOUND:
            case Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR:
            case Poco::Net::HTTPResponse::HTTP_SERVICE_UNAVAILABLE:
                response.setStatusAndReason(static_cast<Poco::Net::HTTPResponse::HTTPStatus>(e.code()));
                response.send();
                break;
            }
        }
        catch (const std::exception &e)
        {
            logger.error("Error in WebSocket handler: %s", std::string(e.what()));
        }
    }

    std::string WebSocketStatusHandler::getStatusJson()
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
            {
                errors_array.add(error.message);
            }
            config_status.set("validation_errors", errors_array);
            status_obj.set("configuration", config_status);

            // Server mode
            auto server_mode = config_manager_->getServerMode("server.mode", ServerMode::EMBEDDING);
            status_obj.set("server_mode", toString(server_mode));
        }

        // Scanned files count
        if (scanned_files_service_)
        {
            status_obj.set("scanned_files_count", scanned_files_service_->count());
            status_obj.set("processed_files_count", scanned_files_service_->countProcessed());
            status_obj.set("error_files_count", scanned_files_service_->countError());
            status_obj.set("queued_files_count", scanned_files_service_->countQueued());
        }

        // Duplicate statistics
        if (duplicate_finder_)
        {
            auto dup_stats = duplicate_finder_->getStats();
            status_obj.set("duplicates_found", dup_stats.total_duplicates);
            status_obj.set("duplicate_groups", dup_stats.total_groups);
            status_obj.set("files_with_duplicates", dup_stats.files_with_duplicates);
        }

        // Registered directories
        if (files_service_)
        {
            auto locations = files_service_->listMediaLocations();
            Poco::JSON::Array directories_array;
            for (const auto &kv : locations)
            {
                directories_array.add(kv.second);
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
                type_obj->set("share", static_cast<double>(kv.second.share));
                type_obj->set("running", static_cast<int>(kv.second.running));
                type_obj->set("queued", static_cast<int>(kv.second.queued));
                type_obj->set("queue_depth", static_cast<int>(tpm_->getQueueDepth(kv.first)));
                per_type_obj->set(kv.first, type_obj);
            }
            tpm_obj.set("per_type", per_type_obj);

            auto all_queue_depths = tpm_->getAllQueueDepths();
            Poco::JSON::Object::Ptr queue_depths_obj = new Poco::JSON::Object();
            for (const auto &kv : all_queue_depths)
            {
                queue_depths_obj->set(kv.first, static_cast<int>(kv.second));
            }
            tpm_obj.set("queue_depths", queue_depths_obj);
            status_obj.set("thread_pool", tpm_obj);
        }

        // Timestamp
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        status_obj.set("timestamp", static_cast<long>(time_t));

        std::stringstream ss;
        status_obj.stringify(ss);
        return ss.str();
    }

} // namespace MediaDedup
