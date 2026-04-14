#include "core/web/web_handlers_duplicates.hpp"
#include "database/database_manager.hpp"
#include "database/user_settings_ops.hpp"
#include "database/scanned_files_ops.hpp"
#include "database/duplicate_groups_ops.hpp"
#include "database/ejected_files_ops.hpp"
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

        ResetDuplicatesHandler::ResetDuplicatesHandler(std::shared_ptr<MediaDedup::DatabaseManager> db)
            : database_manager_(db)
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

        static std::string calculateFileSHA1(const std::string& path) {
            Poco::SHA1Engine sha1;
            Poco::DigestOutputStream dos(sha1);
            Poco::FileInputStream fis(path);
            Poco::StreamCopier::copyStream(fis, dos);
            dos.close();
            return Poco::DigestEngine::digestToHex(sha1.digest());
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
                int success_count = 0;
                int failure_count = 0;
                std::string last_error = "";

                for (size_t i = 0; i < files_array->size(); ++i)
                {
                    std::string file_path = files_array->get(i).toString();
                    auto file_opt = MediaDedup::ScannedFilesOps::getByPath(*database_manager_, file_path);
                    if (!file_opt) {
                        logger.warning("File not found in DB: %s", file_path);
                        last_error = "File not found in database: " + file_path;
                        failure_count++;
                        continue;
                    }

                    std::string location_key = file_opt->location_key;
                    std::string ejection_path;

                    // Ensure ejection path exists for this location_key
                    if (!MediaDedup::UserSettingsOps::get(*database_manager_, "ejectionPath:" + location_key, ejection_path)) {
                        std::string media_location_path;
                        if (!MediaDedup::UserSettingsOps::get(*database_manager_, location_key, media_location_path)) {
                            logger.error("Cannot find media location for key: %s", location_key);
                            last_error = "Cannot find registered media location for key: " + location_key;
                            failure_count++;
                            continue;
                        }
                        Poco::UUID uuid = Poco::UUIDGenerator::defaultGenerator().create();
                        std::filesystem::path root(media_location_path);
                        std::filesystem::path ejected = root / (uuid.toString() + "_ejected");
                        ejection_path = ejected.string();
                        std::replace(ejection_path.begin(), ejection_path.end(), '\\', '/');
                        
                        try {
                            std::filesystem::create_directories(ejected);
                            MediaDedup::UserSettingsOps::upsert(*database_manager_, "ejectionPath:" + location_key, ejection_path);
                        } catch (const std::exception& e) {
                            logger.error("Failed to create ejection directory %s: %s", ejected.string(), std::string(e.what()));
                            last_error = "Failed to create ejection directory: " + std::string(e.what());
                            failure_count++;
                            continue;
                        }
                    } else {
                        // Ensure it exists on disk, recreate if missing
                        std::filesystem::path ejected(ejection_path);
                        if (!std::filesystem::exists(ejected)) {
                            try {
                                std::filesystem::create_directories(ejected);
                            } catch (const std::exception& e) {
                                logger.error("Failed to recreate missing ejection directory %s: %s", ejection_path, std::string(e.what()));
                                last_error = "Failed to recreate missing ejection directory: " + std::string(e.what());
                                failure_count++;
                                continue;
                            }
                        }
                    }

                    // Check if file is already ejected
                    std::string normalized_file_path = file_path;
                    std::replace(normalized_file_path.begin(), normalized_file_path.end(), '\\', '/');
                    if (normalized_file_path.find(ejection_path) == 0) {
                        logger.information("File already in ejection folder: %s", file_path);
                        success_count++; // Treat as success since it's where it belongs
                        continue;
                    }

                    std::filesystem::path src(file_path);
                    if (!std::filesystem::exists(src)) {
                        logger.warning("File does not exist on disk: %s", file_path);
                        last_error = "File does not exist on disk: " + file_path;
                        failure_count++;
                        continue;
                    }

                    Poco::UUID file_uuid = Poco::UUIDGenerator::defaultGenerator().create();
                    std::string dest_filename = src.stem().string() + "_" + file_uuid.toString() + src.extension().string();
                    std::filesystem::path dest = std::filesystem::path(ejection_path) / dest_filename;

                    try {
                        std::filesystem::copy_file(src, dest, std::filesystem::copy_options::overwrite_existing);

                        std::string src_sha1 = calculateFileSHA1(src.string());
                        std::string dest_sha1 = calculateFileSHA1(dest.string());

                        if (src_sha1 == dest_sha1) {
                            std::string new_file_path = dest.string();
                            std::replace(new_file_path.begin(), new_file_path.end(), '\\', '/');

                            std::string new_relative_path;
                            std::string media_location_path;
                            if (MediaDedup::UserSettingsOps::get(*database_manager_, location_key, media_location_path)) {
                                new_relative_path = std::filesystem::relative(dest, std::filesystem::path(media_location_path)).string();
                                std::replace(new_relative_path.begin(), new_relative_path.end(), '\\', '/');
                            }

                            MediaDedup::EjectedFileHistory history;
                            history.file_id = file_opt->id;
                            history.ejected_path = new_file_path;
                            history.original_file_path = file_opt->file_path;
                            history.original_relative_path = file_opt->relative_path;
                            history.original_location_key = location_key;

                            try {
                                auto lease = database_manager_->acquireSessionLease();
                                Poco::Data::Session& sess = lease.get();
                                sess.begin();

                                MediaDedup::EjectedFilesOps::insert(sess, history);
                                MediaDedup::ScannedFilesOps::updateFilePath(sess, file_opt->id, new_file_path, new_relative_path, location_key);
                                MediaDedup::ScannedFilesOps::updateFilePathInImageArtifacts(sess, file_path, new_file_path);
                                MediaDedup::ScannedFilesOps::updateFilePathInProcessingErrors(sess, file_path, new_file_path);
                                MediaDedup::ScannedFilesOps::updateFilePathInDuplicateGroups(sess, file_path, new_file_path);
                                MediaDedup::ScannedFilesOps::updateFilePathInDuplicateMembers(sess, file_path, new_file_path);
                                MediaDedup::ScannedFilesOps::updateFilePathInThumbnailCache(sess, file_path, new_file_path);

                                sess.commit();
                            } catch (...) {
                                std::filesystem::remove(dest);
                                throw;
                            }

                            // Filesystem teardown happens ONLY after DB successfully committed
                            std::filesystem::remove(src);

                            logger.information("Successfully ejected %s to %s", file_path, new_file_path);
                            success_count++;
                        } else {
                            logger.error("SHA1 mismatch after copy for %s", file_path);
                            std::filesystem::remove(dest);
                            last_error = "SHA1 mismatch after copy - file corruption prevented.";
                            failure_count++;
                        }
                    } catch (const std::exception& e) {
                        logger.error("Error copying/ejecting file %s: %s", file_path, std::string(e.what()));
                        last_error = "Error copying file: " + std::string(e.what());
                        failure_count++;
                    }
                }

                response.setStatus(Poco::Net::HTTPResponse::HTTP_OK);
                response.setContentType("application/json");

                Poco::JSON::Object result_obj;
                result_obj.set("success", success_count > 0);
                result_obj.set("success_count", success_count);
                result_obj.set("failure_count", failure_count);
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

                std::vector<MediaDedup::EjectedFileHistory> histories;
                if (undo_all)
                {
                    histories = MediaDedup::EjectedFilesOps::getAll(*database_manager_);
                }
                else
                {
                    for (const auto& f : target_files)
                    {
                        auto hist = MediaDedup::EjectedFilesOps::getByEjectedPath(*database_manager_, f);
                        if (hist)
                        {
                            histories.push_back(*hist);
                        }
                        else
                        {
                            logger.warning("No ejection history found for %s", f);
                        }
                    }
                }

                int success_count = 0;
                int failure_count = 0;

                for (const auto& history : histories)
                {
                    std::string ejected_path = history.ejected_path;
                    std::string original_path = history.original_file_path;

                    std::filesystem::path src(ejected_path);
                    std::filesystem::path dest(original_path);

                    if (!std::filesystem::exists(src))
                    {
                        logger.warning("Ejected file does not exist on disk: %s", ejected_path);
                        MediaDedup::EjectedFilesOps::deleteByEjectedPath(*database_manager_, ejected_path);
                        failure_count++;
                        continue;
                    }

                    try
                    {
                        std::filesystem::copy_file(src, dest, std::filesystem::copy_options::overwrite_existing);

                        std::string src_sha1 = calculateFileSHA1(src.string());
                        std::string dest_sha1 = calculateFileSHA1(dest.string());

                        if (src_sha1 == dest_sha1)
                        {
                            std::string dest_path_str = dest.string();
                            std::replace(dest_path_str.begin(), dest_path_str.end(), '\\', '/');

                            try {
                                auto lease = database_manager_->acquireSessionLease();
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

                            // Filesystem teardown happens ONLY after DB successfully committed
                            std::filesystem::remove(src);

                            // Try to remove ejected directory if empty
                            try {
                                std::filesystem::path parent = src.parent_path();
                                
                                // On macOS, Finders creates .DS_Store instantly, preventing is_empty from returning true
                                std::filesystem::path ds_store = parent / ".DS_Store";
                                if (std::filesystem::exists(ds_store)) {
                                    std::filesystem::remove(ds_store);
                                }

                                if (std::filesystem::is_empty(parent)) {
                                    std::filesystem::remove(parent);
                                    logger.information("Removed empty ejection directory: %s", parent.string());
                                    // Remove the user setting so the UI doesn't try to query a missing folder
                                    MediaDedup::UserSettingsOps::remove(*database_manager_, "ejectionPath:" + history.original_location_key);
                                }
                            } catch (...) {
                                // Ignore directory remove errors
                            }

                            logger.information("Successfully undid ejection %s to %s", ejected_path, dest_path_str);
                            success_count++;
                        }
                        else
                        {
                            logger.error("SHA1 mismatch after copy for undo: %s", ejected_path);
                            std::filesystem::remove(dest);
                            failure_count++;
                        }
                    }
                    catch (const std::exception& e)
                    {
                        logger.error("Error moving file back for undo %s: %s", ejected_path, std::string(e.what()));
                        failure_count++;
                    }
                }

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

    } // namespace Core
} // namespace MediaDedupServer
