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

            Poco::Logger::get("ServerStatusHandler").debug("Basic info set successfully");

            // Configuration status
            if (config_manager_)
            {
                Poco::Logger::get("ServerStatusHandler").debug("Starting config status section");
                Poco::JSON::Object config_status;
                config_status.set("valid", config_manager_->isValid());
                config_status.set("config_file", config_manager_->getConfigFilePath());
                config_status.set("property_count", static_cast<int>(config_manager_->getAllPropertyKeys().size()));

                auto errors = config_manager_->getValidationErrors();
                Poco::Logger::get("ServerStatusHandler").debug("Found %d validation errors", static_cast<int>(errors.size()));
                Poco::JSON::Array errors_array;
                for (const auto &error : errors)
                {
                    Poco::Logger::get("ServerStatusHandler").debug("Adding validation error: %s", error.message);
                    errors_array.add(error.message); // Use the message string from ValidationError
                }
                config_status.set("validation_errors", errors_array);

                status_obj.set("configuration", config_status);
                Poco::Logger::get("ServerStatusHandler").debug("Configuration status set successfully");

                // Server mode
                auto server_mode = config_manager_->getServerMode("server.mode", ServerMode::FAST);
                status_obj.set("server_mode", toString(server_mode));
                Poco::Logger::get("ServerStatusHandler").debug("Server mode set successfully");
            }

            // Scanned files count
            if (scanned_files_service_)
            {
                Poco::Logger::get("ServerStatusHandler").debug("Starting scanned files section");
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

                // Add error count for current server mode only
                if (config_manager_)
                {
                    auto server_mode = config_manager_->getServerMode("server.mode", ServerMode::FAST);
                    int error_count = scanned_files_service_->countError(server_mode);
                    status_obj.set("error_files_count", error_count);
                    Poco::Logger::get("ServerStatusHandler").debug("Error count for current mode: %d", error_count);

                    // Add queued count for current server mode
                    int queued_count = scanned_files_service_->countQueued(server_mode);
                    status_obj.set("queued_files_count", queued_count);
                    Poco::Logger::get("ServerStatusHandler").debug("Queued count for current mode: %d", queued_count);
                }
                else
                {
                    // Fallback to FAST mode if config manager not available
                    int error_count = scanned_files_service_->countError(ServerMode::FAST);
                    status_obj.set("error_files_count", error_count);
                    Poco::Logger::get("ServerStatusHandler").debug("Error count (fallback to FAST mode): %d", error_count);

                    int queued_count = scanned_files_service_->countQueued(ServerMode::FAST);
                    status_obj.set("queued_files_count", queued_count);
                    Poco::Logger::get("ServerStatusHandler").debug("Queued count (fallback to FAST mode): %d", queued_count);
                }
            }

            // Registered directories
            if (files_service_) // TODO: this is not used anywhere
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
                Poco::Logger::get("ServerStatusHandler").debug("Starting thread pool section");
                auto tpm_status = tpm_->getStatus();
                Poco::JSON::Object tpm_obj;
                tpm_obj.set("effective_max_threads", static_cast<int>(tpm_status.effectiveMax));
                tpm_obj.set("running_total", static_cast<int>(tpm_status.runningTotal));

                Poco::JSON::Object::Ptr per_type_obj = new Poco::JSON::Object();
                Poco::Logger::get("ServerStatusHandler").debug("Processing %d thread pool types", static_cast<int>(tpm_status.perType.size()));
                for (const auto &kv : tpm_status.perType)
                {
                    Poco::Logger::get("ServerStatusHandler").debug("Processing thread pool type: %s", kv.first);
                    Poco::JSON::Object::Ptr type_obj = new Poco::JSON::Object();
                    type_obj->set("share", static_cast<double>(kv.second.share));
                    type_obj->set("running", static_cast<int>(kv.second.running));
                    type_obj->set("queued", static_cast<int>(kv.second.queued));

                    // Add current queue depth for this type
                    size_t queue_depth = tpm_->getQueueDepth(kv.first);
                    type_obj->set("queue_depth", static_cast<int>(queue_depth));

                    per_type_obj->set(kv.first, type_obj);
                    Poco::Logger::get("ServerStatusHandler").debug("Thread pool type %s processed successfully", kv.first);
                }
                tpm_obj.set("per_type", per_type_obj);

                // Add overall queue depth summary
                auto all_queue_depths = tpm_->getAllQueueDepths();
                Poco::JSON::Object::Ptr queue_depths_obj = new Poco::JSON::Object();
                for (const auto &kv : all_queue_depths)
                {
                    queue_depths_obj->set(kv.first, static_cast<int>(kv.second));
                }
                tpm_obj.set("queue_depths", queue_depths_obj);
                Poco::Logger::get("ServerStatusHandler").debug("Thread pool per_type set successfully");

                status_obj.set("thread_pool", tpm_obj);
                Poco::Logger::get("ServerStatusHandler").debug("Thread pool status set successfully");
            }

            // Timestamp
            auto now = std::chrono::system_clock::now();
            auto time_t = std::chrono::system_clock::to_time_t(now);
            status_obj.set("timestamp", static_cast<long>(time_t));
            Poco::Logger::get("ServerStatusHandler").debug("Timestamp set successfully");

            // Try to isolate the problematic field by testing individual components
            Poco::Logger::get("ServerStatusHandler").debug("About to stringify JSON object");

            // Test basic fields first
            Poco::JSON::Object test_obj;
            test_obj.set("server_name", "Media Deduplication Server");
            test_obj.set("status", "running");

            std::stringstream test_ss;
            test_obj.stringify(test_ss);
            Poco::Logger::get("ServerStatusHandler").debug("Basic JSON stringification works");

            // Test each complex field individually to isolate the issue
            Poco::Logger::get("ServerStatusHandler").debug("Testing configuration field");
            if (status_obj.has("configuration"))
            {
                try
                {
                    Poco::JSON::Object config_test;
                    config_test.set("test", "value");
                    config_test.set("configuration", status_obj.get("configuration"));
                    std::stringstream config_ss;
                    config_test.stringify(config_ss);
                    Poco::Logger::get("ServerStatusHandler").debug("Configuration field works");
                }
                catch (const std::exception &e)
                {
                    Poco::Logger::get("ServerStatusHandler").debug("Configuration field failed: %s", e.what());
                }
            }

            Poco::Logger::get("ServerStatusHandler").debug("Testing thread_pool field");
            if (status_obj.has("thread_pool"))
            {
                try
                {
                    Poco::JSON::Object tpm_test;
                    tpm_test.set("test", "value");
                    tpm_test.set("thread_pool", status_obj.get("thread_pool"));
                    std::stringstream tpm_ss;
                    tpm_test.stringify(tpm_ss);
                    Poco::Logger::get("ServerStatusHandler").debug("Thread pool field works");
                }
                catch (const std::exception &e)
                {
                    Poco::Logger::get("ServerStatusHandler").debug("Thread pool field failed: %s", e.what());
                }
            }

            // Now try the full object
            std::stringstream ss;
            try
            {
                status_obj.stringify(ss);
                Poco::Logger::get("ServerStatusHandler").debug("JSON stringification completed successfully");
            }
            catch (const std::exception &e)
            {
                Poco::Logger::get("ServerStatusHandler").debug("JSON stringification failed: %s", e.what());
                throw;
            }

            std::string json_str = ss.str();
            Poco::Logger::get("ServerStatusHandler").debug("JSON string length: %d", static_cast<int>(json_str.length()));
            Poco::Logger::get("ServerStatusHandler").debug("About to send JSON response");
            sendJsonResponse(response, json_str);
        }
        catch (const std::exception &e)
        {
            sendErrorResponse(response, "Failed to get server status: " + std::string(e.what()), 500);
        }
    }

} // namespace MediaDedup
