#include "core/web/web_handlers_duplicates.hpp"
#include "orchestration/integrity_check_service.hpp"
#include "database/database_manager.hpp"
#include "database/user_settings_ops.hpp"
#include "database/scanned_files_ops.hpp"
#include "database/duplicate_groups_ops.hpp"
#include "database/ejected_files_ops.hpp"
#include "filesmanager/media_availability.hpp"
#include "orchestration/duplicate_finder.hpp"
#include "orchestration/scheduler_service.hpp"
#include "orchestration/thread_pool_manager.hpp"
#include "core/media_backfill.hpp"
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/URI.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <Poco/JSON/Array.h>
#include <Poco/Logger.h>
#include <Poco/UUIDGenerator.h>
#include <Poco/SHA1Engine.h>
#include <Poco/DigestEngine.h>
#include <Poco/DigestStream.h>
#include <Poco/FileStream.h>
#include <Poco/StreamCopier.h>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <vector>
#include <system_error>
#include <cstddef>
#include <unordered_map>

namespace MediaDedupServer
{
    namespace Core
    {
        DuplicateGroupsHandler::DuplicateGroupsHandler(std::shared_ptr<MediaDedup::DatabaseManager> db)
            : database_manager_(db)
        {
        }

        void DuplicateGroupsHandler::handleRequest(Poco::Net::HTTPServerRequest &request,
                                                   Poco::Net::HTTPServerResponse &response)
        {
            Poco::Logger &logger = Poco::Logger::get("DuplicateGroupsHandler");

            try
            {
                // Parse query parameters
                Poco::URI uri(request.getURI());
                Poco::URI::QueryParameters params = uri.getQueryParameters();

                int start = 0;
                int limit = 100;       // Default limit
                std::string mode = ""; // Empty = all modes

                for (const auto &param : params)
                {
                    if (param.first == "start")
                    {
                        try
                        {
                            start = std::stoi(param.second);
                            if (start < 0)
                                start = 0;
                        }
                        catch (const std::exception &e)
                        {
                            logger.warning("Invalid start parameter: %s", param.second);
                        }
                    }
                    else if (param.first == "limit" || param.first == "end")
                    {
                        try
                        {
                            if (param.first == "end")
                            {
                                // If "end" is provided, calculate limit
                                int end = std::stoi(param.second);
                                limit = end - start;
                            }
                            else
                            {
                                limit = std::stoi(param.second);
                            }

                            // Enforce limits
                            if (limit < 1)
                                limit = 1;
                            if (limit > 1000)
                                limit = 1000; // Maximum 1000 groups per request
                        }
                        catch (const std::exception &e)
                        {
                            logger.warning("Invalid limit/end parameter: %s", param.second);
                        }
                    }
                    else if (param.first == "mode")
                    {
                        // Validate mode (FAST or QUALITY)
                        std::string mode_upper = param.second;
                        std::transform(mode_upper.begin(), mode_upper.end(), mode_upper.begin(), ::toupper);

                        if (mode_upper == "FAST" || mode_upper == "QUALITY")
                        {
                            mode = mode_upper;
                        }
                        else
                        {
                            logger.warning("Invalid mode parameter (must be FAST or QUALITY): %s", param.second);
                        }
                    }
                }

                logger.debug("Retrieving duplicate groups: start=%d, limit=%d, mode=%s", start, limit, mode.empty() ? "all" : mode.c_str());

                // Get groups with members from database (filtered by mode if provided)
                auto page = MediaDedup::DuplicateGroupsOps::getGroupsWithMembers(*database_manager_, start, limit, mode);

                // Fetch ejection paths from user_settings
                auto user_settings = MediaDedup::UserSettingsOps::list(*database_manager_);
                std::vector<std::string> ejection_paths;
                for (const auto& kv : user_settings) {
                    if (kv.first.rfind("ejectionPath:", 0) == 0) {
                        std::string ep = kv.second;
                        std::replace(ep.begin(), ep.end(), '\\', '/');
                        ejection_paths.push_back(ep);
                    }
                }

                // Snapshot the mount table once per request so we can flag members
                // that live on a currently-disconnected volume (cheap, no path I/O).
                auto mounted_fs = MediaDedup::MediaAvailability::mountedFilesystems();

                // Build JSON response
                Poco::JSON::Object root;
                Poco::JSON::Array groups_array;

                for (const auto &group_with_members : page.groups)
                {
                    const auto &group = group_with_members.group;
                    // Exclude single-file groups (should not exist after empty-group fix; defensive)
                    if (group.member_count < 2)
                        continue;

                    Poco::JSON::Object group_obj;
                    group_obj.set("id", group.id);
                    group_obj.set("mode", group.mode);
                    group_obj.set("representative", group.representative_file_path); // Fully qualified path
                    group_obj.set("similarity_threshold", group.similarity_threshold);
                    group_obj.set("member_count", group.member_count);
                    group_obj.set("created_at", group.created_at);
                    group_obj.set("updated_at", group.updated_at);

                    // Add members with detailed information including similarity scores
                    Poco::JSON::Array members_array;
                    for (const auto &member : group_with_members.members)
                    {
                        Poco::JSON::Object member_obj;
                        member_obj.set("file_path", member.file_path); // Fully qualified path
                        member_obj.set("similarity_score", member.similarity_score);
                        member_obj.set("file_size", member.file_size);
                        member_obj.set("created_date", member.created_date);
                        member_obj.set("is_representative", member.is_representative);
                        member_obj.set("added_at", member.added_at);

                        bool is_ejected = false;
                        std::string normalized_file_path = member.file_path;
                        std::replace(normalized_file_path.begin(), normalized_file_path.end(), '\\', '/');
                        for (const auto& ep : ejection_paths) {
                            if (normalized_file_path.find(ep) == 0) {
                                is_ejected = true;
                                break;
                            }
                        }
                        member_obj.set("is_ejected", is_ejected);

                        // Disconnected-media flag: a member is unavailable when the
                        // volume its file lives on is not currently mounted.
                        bool is_available = MediaDedup::MediaAvailability::isPathAvailable(
                            member.file_path, mounted_fs);
                        member_obj.set("is_available", is_available);

                        members_array.add(member_obj);
                    }
                    group_obj.set("members", members_array);

                    groups_array.add(group_obj);
                }

                root.set("groups", groups_array);
                root.set("total_count", page.total_count);
                root.set("start", page.start);
                root.set("end", page.end);
                root.set("returned", static_cast<int>(groups_array.size())); // exclude single-file groups

                // Send response
                response.setStatus(Poco::Net::HTTPResponse::HTTP_OK);
                response.setContentType("application/json");

                std::ostringstream oss;
                root.stringify(oss);
                response.sendBuffer(oss.str().data(), oss.str().length());

                logger.debug("Successfully returned %d duplicate groups", page.returned);
            }
            catch (const std::exception &e)
            {
                logger.error("Exception in DuplicateGroupsHandler: %s", std::string(e.what()));

                response.setStatus(Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);
                response.setContentType("application/json");

                Poco::JSON::Object error_obj;
                error_obj.set("error", "Internal server error");
                error_obj.set("message", e.what());

                std::ostringstream oss;
                error_obj.stringify(oss);
                response.sendBuffer(oss.str().data(), oss.str().length());
            }
        }

        ResetDuplicatesHandler::ResetDuplicatesHandler(std::shared_ptr<MediaDedup::DatabaseManager> db, std::shared_ptr<MediaDedup::Orchestration::SchedulerService> scheduler_service)
            : database_manager_(db), scheduler_service_(scheduler_service)
        {
        }

        void ResetDuplicatesHandler::handleRequest(Poco::Net::HTTPServerRequest &request,
                                                   Poco::Net::HTTPServerResponse &response)
        {
            Poco::Logger &logger = Poco::Logger::get("ResetDuplicatesHandler");

            try
            {
                // Parse query parameters
                Poco::URI uri(request.getURI());
                Poco::URI::QueryParameters params = uri.getQueryParameters();

                std::string mode = ""; // Empty = all modes
                std::vector<std::string> modes_to_reset;

                for (const auto &param : params)
                {
                    if (param.first == "mode")
                    {
                        // Validate mode (currently only EMBEDDING is supported)
                        std::string mode_upper = param.second;
                        std::transform(mode_upper.begin(), mode_upper.end(), mode_upper.begin(), ::toupper);

                        if (mode_upper == "EMBEDDING")
                        {
                            mode = mode_upper;
                            modes_to_reset.push_back(mode_upper);
                            logger.information("Reset requested for mode: %s", mode_upper);
                        }
                        else
                        {
                            logger.warning("Invalid mode parameter (only EMBEDDING is supported): %s", param.second);

                            response.setStatus(Poco::Net::HTTPResponse::HTTP_BAD_REQUEST);
                            response.setContentType("application/json");

                            Poco::JSON::Object error_obj;
                            error_obj.set("success", false);
                            error_obj.set("error", "Invalid mode parameter. Only EMBEDDING mode is supported");

                            std::ostringstream oss;
                            error_obj.stringify(oss);
                            response.sendBuffer(oss.str().data(), oss.str().length());
                            return;
                        }
                    }
                }

                // If no mode specified, reset EMBEDDING mode (the only mode after refactoring)
                if (modes_to_reset.empty())
                {
                    modes_to_reset = {"EMBEDDING"};
                    logger.information("Reset requested for EMBEDDING mode (default)");
                }

                // Perform reset for each mode
                int groups_deleted = 0;
                int checkpoints_reset = 0;

                for (const std::string &reset_mode : modes_to_reset)
                {
                    // Delete groups and members for this mode
                    if (MediaDedup::DuplicateGroupsOps::deleteGroupsByMode(*database_manager_, reset_mode))
                    {
                        groups_deleted++;
                        logger.information("Successfully deleted duplicate groups for mode: %s", reset_mode);
                    }
                    else
                    {
                        logger.error("Failed to delete duplicate groups for mode: %s", reset_mode);
                    }

                    // Reset checkpoint for this mode
                    if (MediaDedup::DuplicateGroupsOps::resetCheckpoint(*database_manager_, reset_mode))
                    {
                        checkpoints_reset++;
                        logger.information("Successfully reset checkpoint for mode: %s", reset_mode);
                    }
                    else
                    {
                        logger.error("Failed to reset checkpoint for mode: %s", reset_mode);
                    }
                }

                // Check if all operations succeeded
                bool success = (groups_deleted == static_cast<int>(modes_to_reset.size())) &&
                               (checkpoints_reset == static_cast<int>(modes_to_reset.size()));

                if (success)
                {
                    logger.information("Duplicate reset completed successfully for %zu mode(s)", modes_to_reset.size());

                    if (scheduler_service_)
                    {
                        logger.information("Triggering duplicate finder out of turn");
                        scheduler_service_->triggerJob("duplicateFinder");
                    }

                    response.setStatus(Poco::Net::HTTPResponse::HTTP_OK);
                    response.setContentType("application/json");

                    Poco::JSON::Object result;
                    result.set("success", true);

                    std::ostringstream oss;
                    result.stringify(oss);
                    response.sendBuffer(oss.str().data(), oss.str().length());
                }
                else
                {
                    logger.error("Duplicate reset failed for one or more modes");

                    response.setStatus(Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);
                    response.setContentType("application/json");

                    Poco::JSON::Object error_obj;
                    error_obj.set("success", false);
                    error_obj.set("error", "Failed to reset one or more modes");

                    std::ostringstream oss;
                    error_obj.stringify(oss);
                    response.sendBuffer(oss.str().data(), oss.str().length());
                }
            }
            catch (const std::exception &e)
            {
                logger.error("Exception in ResetDuplicatesHandler: %s", std::string(e.what()));

                response.setStatus(Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);
                response.setContentType("application/json");

                Poco::JSON::Object error_obj;
                error_obj.set("success", false);
                error_obj.set("error", std::string(e.what()));

                std::ostringstream oss;
                error_obj.stringify(oss);
                response.sendBuffer(oss.str().data(), oss.str().length());
            }
        }

        EjectDuplicatesHandler::EjectDuplicatesHandler(std::shared_ptr<MediaDedup::DatabaseManager> db)
            : database_manager_(db)
        {
        }

        void EjectDuplicatesHandler::handleRequest(Poco::Net::HTTPServerRequest &request,
                                                   Poco::Net::HTTPServerResponse &response)
        {
            Poco::Logger &logger = Poco::Logger::get("EjectDuplicatesHandler");

            try
            {
                if (request.getMethod() != "POST")
                {
                    response.setStatus(Poco::Net::HTTPResponse::HTTP_METHOD_NOT_ALLOWED);
                    response.send();
                    return;
                }

                Poco::JSON::Parser parser;
                Poco::Dynamic::Var result = parser.parse(request.stream());
                Poco::JSON::Object::Ptr object = result.extract<Poco::JSON::Object::Ptr>();

                if (!object->has("files") || !object->isArray("files"))
                {
                    response.setStatus(Poco::Net::HTTPResponse::HTTP_BAD_REQUEST);
                    response.setContentType("application/json");
                    std::string err = "{\"success\":false,\"error\":\"Expected 'files' array\"}";
                    response.sendBuffer(err.data(), err.length());
                    return;
                }

                Poco::JSON::Array::Ptr files_array = object->getArray("files");
                std::vector<std::string> file_paths;
                for (size_t i = 0; i < files_array->size(); ++i) {
                    file_paths.push_back(files_array->get(i).toString());
                }

                BulkOpResult r = bulkEject(*database_manager_, file_paths);
                int success_count = r.successCount;
                int failure_count = r.failureCount;
                int skipped_unavailable = r.skippedUnavailable;
                std::string last_error = r.lastError;

                response.setStatus(Poco::Net::HTTPResponse::HTTP_OK);
                response.setContentType("application/json");

                Poco::JSON::Object result_obj;
                result_obj.set("success", success_count > 0 || (failure_count == 0 && skipped_unavailable > 0));
                result_obj.set("success_count", success_count);
                result_obj.set("failure_count", failure_count);
                result_obj.set("skipped_unavailable", skipped_unavailable);
                if (success_count == 0 && !last_error.empty()) {
                    result_obj.set("error", last_error);
                }

                std::ostringstream oss;
                result_obj.stringify(oss);
                response.sendBuffer(oss.str().data(), oss.str().length());
            }
            catch (const std::exception &e)
            {
                logger.error("Exception in EjectDuplicatesHandler: %s", std::string(e.what()));

                response.setStatus(Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);
                response.setContentType("application/json");

                Poco::JSON::Object error_obj;
                error_obj.set("success", false);
                error_obj.set("error", std::string(e.what()));

                std::ostringstream oss;
                error_obj.stringify(oss);
                response.sendBuffer(oss.str().data(), oss.str().length());
            }
        }

        UndoEjectDuplicatesHandler::UndoEjectDuplicatesHandler(std::shared_ptr<MediaDedup::DatabaseManager> db)
            : database_manager_(db)
        {
        }

        void UndoEjectDuplicatesHandler::handleRequest(Poco::Net::HTTPServerRequest &request,
                                                       Poco::Net::HTTPServerResponse &response)
        {
            Poco::Logger &logger = Poco::Logger::get("UndoEjectDuplicatesHandler");

            try
            {
                if (request.getMethod() != "POST")
                {
                    response.setStatus(Poco::Net::HTTPResponse::HTTP_METHOD_NOT_ALLOWED);
                    response.send();
                    return;
                }

                Poco::JSON::Parser parser;
                Poco::Dynamic::Var result = parser.parse(request.stream());
                Poco::JSON::Object::Ptr object = result.extract<Poco::JSON::Object::Ptr>();

                bool undo_all = object->optValue("undo_all", false);
                std::vector<std::string> target_files;

                if (!undo_all)
                {
                    if (!object->has("files") || !object->isArray("files"))
                    {
                        response.setStatus(Poco::Net::HTTPResponse::HTTP_BAD_REQUEST);
                        response.setContentType("application/json");
                        std::string err = "{\"success\":false,\"error\":\"Expected 'files' array or 'undo_all' boolean\"}";
                        response.sendBuffer(err.data(), err.length());
                        return;
                    }

                    Poco::JSON::Array::Ptr files_array = object->getArray("files");
                    for (size_t i = 0; i < files_array->size(); ++i)
                    {
                        target_files.push_back(files_array->get(i).toString());
                    }
                }

                BulkOpResult r = bulkUndoEject(*database_manager_, target_files, undo_all);
                int success_count = r.successCount;
                int failure_count = r.failureCount;
                int skipped_unavailable = r.skippedUnavailable;

                response.setStatus(Poco::Net::HTTPResponse::HTTP_OK);
                response.setContentType("application/json");

                Poco::JSON::Object result_obj;
                result_obj.set("success", success_count > 0 || (failure_count == 0));
                result_obj.set("success_count", success_count);
                result_obj.set("failure_count", failure_count);
                result_obj.set("skipped_unavailable", skipped_unavailable);

                std::ostringstream oss;
                result_obj.stringify(oss);
                response.sendBuffer(oss.str().data(), oss.str().length());
            }
            catch (const std::exception &e)
            {
                logger.error("Exception in UndoEjectDuplicatesHandler: %s", std::string(e.what()));

                response.setStatus(Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);
                response.setContentType("application/json");

                Poco::JSON::Object error_obj;
                error_obj.set("success", false);
                error_obj.set("error", std::string(e.what()));

                std::ostringstream oss;
                error_obj.stringify(oss);
                response.sendBuffer(oss.str().data(), oss.str().length());
            }
        }

        PurgeEjectedErroredHandler::PurgeEjectedErroredHandler(std::shared_ptr<MediaDedup::DatabaseManager> db, std::shared_ptr<MediaDedup::UnifiedObservableConfigManager> cfg)
            : database_manager_(db), config_manager_(cfg)
        {
        }

        void PurgeEjectedErroredHandler::handleRequest(Poco::Net::HTTPServerRequest &request,
                                                       Poco::Net::HTTPServerResponse &response)
        {
            Poco::Logger &logger = Poco::Logger::get("PurgeEjectedErroredHandler");

            try
            {
                if (request.getMethod() != "POST")
                {
                    response.setStatus(Poco::Net::HTTPResponse::HTTP_METHOD_NOT_ALLOWED);
                    response.send();
                    return;
                }

                // AR-2: shared purge (rep promotion + member-count sync +
                // all three history kinds) lives in the eject service.
                auto pr = MediaDedup::Services::purgeEjected(*database_manager_, config_manager_);
                int success_count = pr.successCount;
                int failure_count = pr.failureCount;

                response.setStatus(Poco::Net::HTTPResponse::HTTP_OK);
                response.setContentType("application/json");

                Poco::JSON::Object result_obj;
                result_obj.set("success", true);
                result_obj.set("success_count", success_count);
                result_obj.set("failure_count", failure_count);

                std::ostringstream oss;
                result_obj.stringify(oss);
                response.sendBuffer(oss.str().data(), oss.str().length());
            }
            catch (const std::exception &e)
            {
                logger.error("Exception in PurgeEjectedErroredHandler: %s", std::string(e.what()));

                response.setStatus(Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);
                response.setContentType("application/json");

                Poco::JSON::Object error_obj;
                error_obj.set("success", false);
                error_obj.set("error", std::string(e.what()));

                std::ostringstream oss;
                error_obj.stringify(oss);
                response.sendBuffer(oss.str().data(), oss.str().length());
            }
        }

        // --- DatabaseIntegrityCheckHandler ---
        // AR-1: the check itself lives in Orchestration::IntegrityCheckService;
        // this handler is HTTP plumbing only (the gRPC surface calls the
        // service directly instead of reaching up into core/web).

        DatabaseIntegrityCheckHandler::DatabaseIntegrityCheckHandler(
            std::shared_ptr<MediaDedup::DatabaseManager> db, 
            std::shared_ptr<MediaDedup::UnifiedObservableConfigManager> cfg,
            std::shared_ptr<MediaDedup::Orchestration::SchedulerService> scheduler,
            std::shared_ptr<MediaDedup::ThreadPoolManager> tpm)
            : database_manager_(db), config_manager_(cfg), scheduler_service_(scheduler), tpm_(tpm)
        {
        }

        void DatabaseIntegrityCheckHandler::handleRequest(Poco::Net::HTTPServerRequest &request,
                                                          Poco::Net::HTTPServerResponse &response)
        {
            if (request.getURI().find("/start") != std::string::npos && request.getMethod() == "POST") {
                handleStart(response);
            } else if (request.getURI().find("/status") != std::string::npos && request.getMethod() == "GET") {
                handleStatus(response);
            } else {
                response.setStatus(Poco::Net::HTTPResponse::HTTP_METHOD_NOT_ALLOWED);
                response.send();
            }
        }

        void DatabaseIntegrityCheckHandler::handleStart(Poco::Net::HTTPServerResponse &response)
        {
            std::string error;
            if (!MediaDedup::Orchestration::IntegrityCheckService::start(
                    database_manager_, config_manager_, scheduler_service_, tpm_, error)) {
                response.setStatus(Poco::Net::HTTPResponse::HTTP_CONFLICT);
                response.setContentType("application/json");
                std::string err = R"({"success":false,"error":")" + error + R"("})";
                response.sendBuffer(err.data(), err.length());
                return;
            }

            response.setStatus(Poco::Net::HTTPResponse::HTTP_OK);
            response.setContentType("application/json");
            std::string res = R"({"success":true,"message":"Integrity check started"})";
            response.sendBuffer(res.data(), res.length());
        }

        void DatabaseIntegrityCheckHandler::handleStatus(Poco::Net::HTTPServerResponse &response)
        {
            auto current_state = MediaDedup::Orchestration::IntegrityCheckService::status();
            Poco::JSON::Object result_obj;
            result_obj.set("running", (bool)current_state.is_running);
            result_obj.set("progress", (int)current_state.progress);
            result_obj.set("total_files", (int)current_state.total_files);
            result_obj.set("processed_files", (int)current_state.processed_files);
            result_obj.set("files_removed", (int)current_state.files_removed);
            result_obj.set("groups_removed", (int)current_state.groups_removed);
            result_obj.set("thumbnails_removed", (int)current_state.thumbnails_removed);
            result_obj.set("artifacts_removed", (int)current_state.artifacts_removed);
            result_obj.set("errors_removed", (int)current_state.errors_removed);
            result_obj.set("current_phase", current_state.current_phase);

            response.setStatus(Poco::Net::HTTPResponse::HTTP_OK);
            response.setContentType("application/json");
            std::ostringstream oss;
            result_obj.stringify(oss);
            response.sendBuffer(oss.str().data(), oss.str().length());
        }

        BackupDatabaseHandler::BackupDatabaseHandler(std::shared_ptr<MediaDedup::DatabaseManager> db)
            : database_manager_(db) {}

        void BackupDatabaseHandler::handleRequest(Poco::Net::HTTPServerRequest &request,
                                                  Poco::Net::HTTPServerResponse &response)
        {
            response.setContentType("application/json");

            try
            {
                std::string db_path = database_manager_->getDatabasePath();
                if (db_path.empty()) {
                    response.setStatus(Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);
                    std::ostream &out = response.send();
                    out << "{\"success\": false, \"error\": \"Database path is empty\"}";
                    return;
                }

                // Generate backup directory: <db_path>_backup_YYYYMMDDHHMMSS/
                std::filesystem::path p(db_path);
                std::string stem = p.stem().string();
                std::string ext = p.extension().string();

                auto now = std::chrono::system_clock::now();
                auto time_t = std::chrono::system_clock::to_time_t(now);
                std::stringstream ss;
                ss << std::put_time(std::localtime(&time_t), "%Y%m%d%H%M%S");
                std::string timestamp = ss.str();

                std::string backup_dir_name = stem + "_backup_" + timestamp;
                std::filesystem::path backup_dir = p.parent_path() / backup_dir_name;
                
                // Create the directory
                std::filesystem::create_directories(backup_dir);

                // Place the backup inside with its original name
                std::filesystem::path backup_path = backup_dir / (stem + ext);

                bool success = database_manager_->backupDatabase(backup_path.string());

                if (success)
                {
                    std::string zip_path = backup_dir.string() + ".zip";
                    std::string cmd = "cd \"" + p.parent_path().string() + "\" && zip -r -q \"" + backup_dir_name + ".zip\" \"" + backup_dir_name + "\" && rm -rf \"" + backup_dir_name + "\"";
                    int result = std::system(cmd.c_str());
                    
                    if (result == 0) {
                        backup_path = p.parent_path() / (backup_dir_name + ".zip");
                    } else {
                        Poco::Logger::get("BackupDatabaseHandler").warning("Compression failed, keeping uncompressed directory.");
                    }

                    response.setStatus(Poco::Net::HTTPResponse::HTTP_OK);
                    std::ostream &out = response.send();
                    out << "{\"success\": true, \"backup_path\": \"" << backup_path.string() << "\"}";
                }
                else
                {
                    response.setStatus(Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);
                    std::ostream &out = response.send();
                    out << "{\"success\": false, \"error\": \"Failed to backup database\"}";
                }
            }
            catch (const std::exception &e)
            {
                Poco::Logger::get("BackupDatabaseHandler").error("Error: %s", std::string(e.what()));
                response.setStatus(Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);
                std::ostream &out = response.send();
                out << "{\"success\": false, \"error\": \"" << e.what() << "\"}";
            }
        }

        GetLatestBackupHandler::GetLatestBackupHandler(std::shared_ptr<MediaDedup::DatabaseManager> db)
            : database_manager_(db)
        {
        }

        void GetLatestBackupHandler::handleRequest(Poco::Net::HTTPServerRequest &request, Poco::Net::HTTPServerResponse &response)
        {
            response.setContentType("application/json");
            try
            {
                std::string db_path = database_manager_->getDatabasePath();
                if (db_path.empty()) {
                    response.setStatus(Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);
                    std::ostream &out = response.send();
                    out << "{\"success\": false, \"error\": \"Database path is empty\"}";
                    return;
                }

                std::filesystem::path p(db_path);
                std::string stem = p.stem().string();
                std::string prefix = stem + "_backup_";
                std::filesystem::path parent = p.parent_path();
                std::string latest_backup_file = "";
                std::string latest_dir_name = "";

                if (std::filesystem::exists(parent) && std::filesystem::is_directory(parent)) {
                    for (const auto& entry : std::filesystem::directory_iterator(parent)) {
                        std::string filename = entry.path().filename().string();
                        if (filename.find(prefix) == 0) {
                            if (entry.is_directory()) {
                                std::filesystem::path backup_file = entry.path() / p.filename();
                                if (std::filesystem::exists(backup_file)) {
                                    if (latest_dir_name.empty() || filename > latest_dir_name) {
                                        latest_dir_name = filename;
                                        latest_backup_file = backup_file.string();
                                    }
                                }
                            } else if (filename.size() >= 4 && filename.substr(filename.size() - 4) == ".zip") {
                                std::string dir_name = filename.substr(0, filename.size() - 4);
                                if (latest_dir_name.empty() || dir_name > latest_dir_name) {
                                    latest_dir_name = dir_name;
                                    latest_backup_file = entry.path().string();
                                }
                            }
                        }
                    }
                }

                response.setStatus(Poco::Net::HTTPResponse::HTTP_OK);
                std::ostream &out = response.send();
                if (latest_backup_file.empty()) {
                    out << "{\"success\": true, \"backup_path\": null}";
                } else {
                    out << "{\"success\": true, \"backup_path\": \"" << latest_backup_file << "\"}";
                }
            }
            catch (const std::exception &e)
            {
                Poco::Logger::get("GetLatestBackupHandler").error("Error: %s", std::string(e.what()));
                response.setStatus(Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);
                std::ostream &out = response.send();
                out << "{\"success\": false, \"error\": \"" << e.what() << "\"}";
            }
        }

    } // namespace Core
} // namespace MediaDedupServer
