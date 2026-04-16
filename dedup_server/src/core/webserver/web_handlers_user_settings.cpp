/*
 * File: core/webserver/web_handlers_user_settings.cpp
 * Purpose: Implements user settings API handlers backed by SQLite service.
 * Endpoints:
 *   - GET /api/v1/user-settings
 *   - GET/PUT/DELETE /api/v1/user-settings/{key}
 */
#include "core/web/web_server.hpp"
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <sstream>
#include <regex>
#include "database/user_settings_service.hpp"
#include "filesmanager/files_service.hpp"
#include "orchestration/thread_pool_manager.hpp"
#include "database/duplicate_groups_ops.hpp"
#include "database/ejected_files_ops.hpp"
#include "database/database_manager.hpp"
#include "database/scanned_files_ops.hpp"
#include "database/user_settings_ops.hpp"
#include <Poco/SHA1Engine.h>
#include <Poco/DigestEngine.h>
#include <Poco/DigestStream.h>
#include <Poco/FileStream.h>
#include <Poco/StreamCopier.h>
#include <filesystem>

namespace MediaDedup
{
    static std::string calculateFileSHA1_UserSettings(const std::string& path) {
        Poco::SHA1Engine sha1;
        Poco::DigestOutputStream dos(sha1);
        Poco::FileInputStream fis(path);
        Poco::StreamCopier::copyStream(fis, dos);
        dos.close();
        return Poco::DigestEngine::digestToHex(sha1.digest());
    }

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

    ConfigurePurityOrderHandler::ConfigurePurityOrderHandler(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                                                             std::shared_ptr<DatabaseManager> db_manager)
        : ConfigRequestHandler(std::move(config_manager)), db_manager_(std::move(db_manager)) {}

    void ConfigurePurityOrderHandler::handleRequest(Poco::Net::HTTPServerRequest &request,
                                                    Poco::Net::HTTPServerResponse &response)
    {
        Poco::Logger::get("ConfigurePurityOrderHandler").information("Request received to configure purity order and wipe groups");

        if (request.getMethod() != "POST")
        {
            sendErrorResponse(response, "Method not allowed", 405);
            return;
        }

        std::string body = getRequestBody(request);
        if (body.empty())
        {
            sendErrorResponse(response, "Body required", 400);
            return;
        }

        Poco::JSON::Parser parser;
        Poco::JSON::Object::Ptr json;
        try
        {
            json = parser.parse(body).extract<Poco::JSON::Object::Ptr>();
        }
        catch (...)
        {
            sendErrorResponse(response, "Invalid JSON body", 400);
            return;
        }

        if (!json->has("purity_order"))
        {
            sendErrorResponse(response, "Missing 'purity_order' in JSON", 400);
            return;
        }

        std::string new_order = json->get("purity_order").toString();
        if (new_order.empty())
        {
            sendErrorResponse(response, "purity_order cannot be empty", 400);
            return;
        }

        if (!config_manager_ || !db_manager_)
        {
            sendErrorResponse(response, "Service unavailable", 503);
            return;
        }

        try
        {
            Poco::Logger::get("ConfigurePurityOrderHandler").information("Bulk restoring ejected items...");
            {
                auto histories = MediaDedup::EjectedFilesOps::getAll(*db_manager_);
                for (const auto& history : histories)
                {
                    std::string ejected_path = history.ejected_path;
                    std::string original_path = history.original_file_path;

                    std::filesystem::path src(ejected_path);
                    std::filesystem::path dest(original_path);

                    if (!std::filesystem::exists(src))
                    {
                        MediaDedup::EjectedFilesOps::deleteByEjectedPath(*db_manager_, ejected_path);
                        continue;
                    }

                    try
                    {
                        std::filesystem::copy_file(src, dest, std::filesystem::copy_options::overwrite_existing);

                        std::string src_sha1 = calculateFileSHA1_UserSettings(src.string());
                        std::string dest_sha1 = calculateFileSHA1_UserSettings(dest.string());

                        if (src_sha1 == dest_sha1)
                        {
                            std::string dest_path_str = dest.string();
                            std::replace(dest_path_str.begin(), dest_path_str.end(), '\\', '/');

                            try {
                                auto lease = db_manager_->acquireSessionLease();
                                Poco::Data::Session& sess = lease.get();
                                sess.begin();

                                MediaDedup::ScannedFilesOps::updateFilePath(sess, history.file_id, dest_path_str, history.original_relative_path, history.original_location_key);
                                MediaDedup::ScannedFilesOps::updateFilePathInImageArtifacts(sess, ejected_path, dest_path_str);
                                MediaDedup::ScannedFilesOps::updateFilePathInProcessingErrors(sess, ejected_path, dest_path_str);
                                MediaDedup::ScannedFilesOps::updateFilePathInDuplicateGroups(sess, ejected_path, dest_path_str);
                                MediaDedup::ScannedFilesOps::updateFilePathInDuplicateMembers(sess, ejected_path, dest_path_str);
                                MediaDedup::ScannedFilesOps::updateFilePathInThumbnailCache(sess, ejected_path, dest_path_str);

                                MediaDedup::EjectedFilesOps::deleteByEjectedPath(sess, ejected_path);

                                sess.commit();
                            } catch (...) {
                                std::filesystem::remove(dest);
                                throw;
                            }

                            std::filesystem::remove(src);
                        }
                        else
                        {
                            std::filesystem::remove(dest);
                        }
                    }
                    catch (const std::exception& e)
                    {
                        Poco::Logger::get("ConfigurePurityOrderHandler").error("Error restoring %s: %s", ejected_path, std::string(e.what()));
                    }
                }
            }

            Poco::Logger::get("ConfigurePurityOrderHandler").information("Wiping duplicate groups for EMBEDDING mode...");
            DuplicateGroupsOps::deleteGroupsByMode(*db_manager_, "EMBEDDING");

            Poco::Logger::get("ConfigurePurityOrderHandler").information("Resetting checkpoint and regenerating...");
            DuplicateGroupsOps::resetCheckpoint(*db_manager_, "EMBEDDING");

            Poco::Logger::get("ConfigurePurityOrderHandler").information("Writing config.yaml with new order: %s", new_order);
            config_manager_->setPropertyValue<std::string>("duplicates.purity.order", new_order);
            config_manager_->saveConfiguration();

            Poco::JSON::Object obj;
            obj.set("status", "ok");
            obj.set("message", "Purity order configured, duplicate groups wiped, and scanner reschedueled.");
            
            std::ostringstream oss;
            obj.stringify(oss);
            sendJsonResponse(response, oss.str());
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ConfigurePurityOrderHandler").error("Error during wipe and rebuild: %s", std::string(e.what()));
            sendErrorResponse(response, std::string("Internal error: ") + e.what(), 500);
        }
    }

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
        obj.set("immediate_scan_triggered", service_->isImmediateJobTriggerEnabled());
        std::ostringstream oss;
        obj.stringify(oss);
        sendJsonResponse(response, oss.str());
    }

    DeregisterMediaLocationHandler::DeregisterMediaLocationHandler(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                                                                   std::shared_ptr<FilesService> service,
                                                                   std::shared_ptr<ThreadPoolManager> tpm)
        : ConfigRequestHandler(std::move(config_manager)), service_(std::move(service)), tpm_(std::move(tpm)) {}
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

        if (tpm_)
        {
            Poco::Logger::get("DeregisterHandler").information("Clearing thread pool queues prior to deregistering a path.");
            tpm_->clearQueues();
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

    ChangeMediaLocationPathHandler::ChangeMediaLocationPathHandler(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                                                                   std::shared_ptr<FilesService> service)
        : ConfigRequestHandler(std::move(config_manager)), service_(std::move(service)) {}

    void ChangeMediaLocationPathHandler::handleRequest(Poco::Net::HTTPServerRequest &request,
                                                       Poco::Net::HTTPServerResponse &response)
    {
        Poco::Logger::get("ChangeMediaLocationPathHandler").information("ChangeMediaLocationPathHandler: Request received");

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
        Poco::Logger::get("ChangeMediaLocationPathHandler").information("ChangeMediaLocationPathHandler: Body received, length=%d", static_cast<int>(body.length()));

        Poco::JSON::Parser parser;
        Poco::JSON::Object::Ptr json;
        try
        {
            json = parser.parse(body).extract<Poco::JSON::Object::Ptr>();
        }
        catch (...)
        {
            sendErrorResponse(response, "Invalid JSON", 400);
            return;
        }
        if (!json->has("old_path") || !json->has("new_path"))
        {
            sendErrorResponse(response, "Missing old_path or new_path", 400);
            return;
        }
        std::string old_path = json->get("old_path").toString();
        std::string new_path = json->get("new_path").toString();
        Poco::Logger::get("ChangeMediaLocationPathHandler").information("ChangeMediaLocationPathHandler: Calling changeMediaLocationPath: old_path=%s, new_path=%s", old_path, new_path);

        if (old_path.empty() || new_path.empty())
        {
            sendErrorResponse(response, "old_path and new_path cannot be empty", 400);
            return;
        }
        int sample_size = 20;
        if (json->has("sample_size"))
        {
            sample_size = json->get("sample_size").convert<int>();
            if (sample_size < 1 || sample_size > 100)
            {
                sendErrorResponse(response, "sample_size must be between 1 and 100", 400);
                return;
            }
        }
        auto result = service_->changeMediaLocationPath(old_path, new_path, sample_size);
        Poco::JSON::Object obj;
        if (result.success)
        {
            obj.set("status", "ok");
        }
        else if (result.partial_success)
        {
            obj.set("status", "partial_success");
        }
        else
        {
            obj.set("status", "error");
        }
        obj.set("old_path", old_path);
        obj.set("new_path", new_path);
        obj.set("files_verified", result.files_verified);
        obj.set("files_verified_success", result.files_verified_success);
        obj.set("total_files", result.total_files);
        obj.set("files_updated", result.files_updated);
        obj.set("files_failed", result.files_failed);
        obj.set("verification_success_rate", result.verification_success_rate);
        if (!result.error_message.empty())
        {
            obj.set("error", result.error_message);
        }
        // Add update details
        Poco::JSON::Object details_obj;
        for (const auto &kv : result.update_details)
        {
            Poco::JSON::Object table_obj;
            table_obj.set("updated", kv.second.first);
            table_obj.set("failed", kv.second.second);
            details_obj.set(kv.first, table_obj);
        }
        obj.set("update_details", details_obj);
        std::ostringstream oss;
        obj.stringify(oss);
        if (result.success)
        {
            sendJsonResponse(response, oss.str());
        }
        else if (result.partial_success)
        {
            response.setStatus(Poco::Net::HTTPResponse::HTTP_OK);
            response.setContentType("application/json");
            std::ostringstream response_stream;
            obj.stringify(response_stream);
            response.setContentLength(response_stream.str().length());
            response.send() << response_stream.str();
        }
        else
        {
            if (result.error_message.find("not registered") != std::string::npos)
            {
                sendErrorResponse(response, result.error_message, 404);
            }
            else if (result.error_message.find("does not exist") != std::string::npos ||
                     result.error_message.find("Verification failed") != std::string::npos)
            {
                sendErrorResponse(response, result.error_message, 400);
            }
            else
            {
                sendErrorResponse(response, result.error_message, 500);
            }
        }
    }

    VerifyPathRemappingHandler::VerifyPathRemappingHandler(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                                                           std::shared_ptr<FilesService> service)
        : ConfigRequestHandler(std::move(config_manager)), service_(std::move(service)) {}

    void VerifyPathRemappingHandler::handleRequest(Poco::Net::HTTPServerRequest &request,
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
        Poco::JSON::Object::Ptr json;
        try
        {
            json = parser.parse(body).extract<Poco::JSON::Object::Ptr>();
        }
        catch (...)
        {
            sendErrorResponse(response, "Invalid JSON", 400);
            return;
        }
        if (!json->has("old_path") || !json->has("new_path"))
        {
            sendErrorResponse(response, "Missing old_path or new_path", 400);
            return;
        }
        std::string old_path = json->get("old_path").toString();
        std::string new_path = json->get("new_path").toString();

        if (old_path.empty() || new_path.empty())
        {
            sendErrorResponse(response, "old_path and new_path cannot be empty", 400);
            return;
        }
        int sample_size = 20;
        if (json->has("sample_size"))
        {
            sample_size = json->get("sample_size").convert<int>();
            if (sample_size < 1 || sample_size > 100)
            {
                sendErrorResponse(response, "sample_size must be between 1 and 100", 400);
                return;
            }
        }

        auto verification = service_->verifyPathRemapping(old_path, new_path, sample_size);

        Poco::JSON::Object obj;
        obj.set("is_successful", verification.is_successful);
        obj.set("old_path", old_path);
        obj.set("new_path", new_path);
        obj.set("old_location_key", verification.old_location_key);
        obj.set("new_location_key", verification.new_location_key);
        obj.set("old_location_key_file_count", verification.old_location_key_file_count);
        obj.set("new_location_key_file_count", verification.new_location_key_file_count);
        obj.set("files_with_old_path_prefix", verification.files_with_old_path_prefix);
        obj.set("files_with_new_path_prefix", verification.files_with_new_path_prefix);
        obj.set("sampled_files_verified", verification.sampled_files_verified);
        obj.set("sampled_files_total", verification.sampled_files_total);
        obj.set("old_location_registered", verification.old_location_registered);
        obj.set("new_location_registered", verification.new_location_registered);
        obj.set("is_in_progress", verification.is_in_progress);
        if (!verification.error_message.empty())
        {
            obj.set("error", verification.error_message);
        }

        std::ostringstream oss;
        obj.stringify(oss);
        sendJsonResponse(response, oss.str());
    }

} // namespace MediaDedup
