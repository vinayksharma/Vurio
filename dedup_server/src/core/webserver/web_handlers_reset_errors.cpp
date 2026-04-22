#include "core/web/web_handlers_reset_errors.hpp"
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

    static std::string calculateFileSHA1(const std::string& path) {
        Poco::SHA1Engine sha1;
        Poco::DigestOutputStream dos(sha1);
        Poco::FileInputStream fis(path);
        Poco::StreamCopier::copyStream(fis, dos);
        dos.close();
        return Poco::DigestEngine::digestToHex(sha1.digest());
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
            auto errored_files = ScannedFilesOps::listErroredFiles(*database_manager_);

            int success_count = 0;
            int failure_count = 0;
            std::string last_error = "";

            for (const auto& file_opt : errored_files)
            {
                std::string file_path = file_opt.file_path;
                std::string location_key = file_opt.location_key;
                std::string ejection_path;

                if (!UserSettingsOps::get(*database_manager_, "ejectionPathErrored:" + location_key, ejection_path)) {
                    std::string media_location_path;
                    if (!UserSettingsOps::get(*database_manager_, location_key, media_location_path)) {
                        last_error = "Cannot find registered media location for key: " + location_key;
                        failure_count++;
                        continue;
                    }
                    Poco::UUID uuid = Poco::UUIDGenerator::defaultGenerator().create();
                    std::filesystem::path root(media_location_path);
                    std::filesystem::path ejected = root / (uuid.toString() + "_errored");
                    ejection_path = ejected.string();
                    std::replace(ejection_path.begin(), ejection_path.end(), '\\', '/');
                    
                    try {
                        std::filesystem::create_directories(ejected);
                        UserSettingsOps::upsert(*database_manager_, "ejectionPathErrored:" + location_key, ejection_path);
                    } catch (const std::exception& e) {
                        last_error = "Failed to create ejection directory: " + std::string(e.what());
                        failure_count++;
                        continue;
                    }
                } else {
                    std::filesystem::path ejected(ejection_path);
                    if (!std::filesystem::exists(ejected)) {
                        try {
                            std::filesystem::create_directories(ejected);
                        } catch (const std::exception& e) {
                            last_error = "Failed to recreate missing ejection directory: " + std::string(e.what());
                            failure_count++;
                            continue;
                        }
                    }
                }

                std::string normalized_file_path = file_path;
                std::replace(normalized_file_path.begin(), normalized_file_path.end(), '\\', '/');
                if (normalized_file_path.find(ejection_path) == 0) {
                    success_count++;
                    continue;
                }

                std::filesystem::path src(file_path);
                if (!std::filesystem::exists(src)) {
                    last_error = "File does not exist on disk: " + file_path;
                    failure_count++;
                    continue;
                }

                std::string dest_filename = src.filename().string();
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
                        if (UserSettingsOps::get(*database_manager_, location_key, media_location_path)) {
                            new_relative_path = std::filesystem::relative(dest, std::filesystem::path(media_location_path)).string();
                            std::replace(new_relative_path.begin(), new_relative_path.end(), '\\', '/');
                        }

                        EjectedFileHistory history;
                        history.file_id = file_opt.id;
                        history.ejected_path = new_file_path;
                        history.original_file_path = file_opt.file_path;
                        history.original_relative_path = file_opt.relative_path;
                        history.original_location_key = location_key;

                        try {
                            auto lease = database_manager_->acquireSessionLease();
                            Poco::Data::Session& sess = lease.get();
                            sess.begin();

                            EjectedFilesOps::insert(sess, history);
                            ScannedFilesOps::updateFilePath(sess, file_opt.id, new_file_path, new_relative_path, location_key);
                            ScannedFilesOps::updateFilePathInImageArtifacts(sess, file_path, new_file_path);
                            ScannedFilesOps::updateFilePathInProcessingErrors(sess, file_path, new_file_path);
                            ScannedFilesOps::updateFilePathInDuplicateGroups(sess, file_path, new_file_path);
                            ScannedFilesOps::updateFilePathInDuplicateMembers(sess, file_path, new_file_path);
                            ScannedFilesOps::updateFilePathInThumbnailCache(sess, file_path, new_file_path);

                            sess.commit();
                        } catch (...) {
                            std::filesystem::remove(dest);
                            throw;
                        }

                        std::filesystem::remove(src);
                        success_count++;
                    } else {
                        std::filesystem::remove(dest);
                        last_error = "SHA1 mismatch after copy - file corruption prevented.";
                        failure_count++;
                    }
                } catch (const std::exception& e) {
                    last_error = "Error copying file: " + std::string(e.what());
                    failure_count++;
                }
            }

            Poco::JSON::Object result_obj;
            result_obj.set("success", success_count > 0 || (success_count == 0 && failure_count == 0));
            result_obj.set("success_count", success_count);
            result_obj.set("failure_count", failure_count);
            if (success_count == 0 && !last_error.empty()) {
                result_obj.set("error", last_error);
            }

            std::ostringstream oss;
            result_obj.stringify(oss);
            sendJsonResponse(response, oss.str());
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
            auto all_histories = EjectedFilesOps::getAll(*database_manager_);
            std::vector<EjectedFileHistory> histories;
            for (const auto& hist : all_histories) {
                if (hist.ejected_path.find("_errored") != std::string::npos) {
                    histories.push_back(hist);
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
                    EjectedFilesOps::deleteByEjectedPath(*database_manager_, ejected_path);
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

                            ScannedFilesOps::updateFilePath(sess, history.file_id, dest_path_str, history.original_relative_path, history.original_location_key);
                            ScannedFilesOps::updateFilePathInImageArtifacts(sess, ejected_path, dest_path_str);
                            ScannedFilesOps::updateFilePathInProcessingErrors(sess, ejected_path, dest_path_str);
                            ScannedFilesOps::updateFilePathInDuplicateGroups(sess, ejected_path, dest_path_str);
                            ScannedFilesOps::updateFilePathInDuplicateMembers(sess, ejected_path, dest_path_str);
                            ScannedFilesOps::updateFilePathInThumbnailCache(sess, ejected_path, dest_path_str);

                            // The user requested that we treat them as if we just cleared the errors.
                            // Set `processed=0` and remove from `processing_errors`.
                            std::string dest_p = dest_path_str;
                            Poco::Data::Statement stmt_update_state(sess);
                            int unproc = 0;
                            stmt_update_state << "UPDATE scanned_files SET processed=? WHERE file_path=?", Poco::Data::Keywords::use(unproc), Poco::Data::Keywords::use(dest_p), Poco::Data::Keywords::now;

                            Poco::Data::Statement stmt_del_err(sess);
                            stmt_del_err << "DELETE FROM processing_errors WHERE file_path=?", Poco::Data::Keywords::use(dest_p), Poco::Data::Keywords::now;

                            EjectedFilesOps::deleteByEjectedPath(sess, ejected_path);

                            sess.commit();
                        } catch (...) {
                            std::filesystem::remove(dest);
                            throw;
                        }

                        std::filesystem::remove(src);

                        try {
                            std::filesystem::path parent = src.parent_path();
                            std::filesystem::path ds_store = parent / ".DS_Store";
                            if (std::filesystem::exists(ds_store)) {
                                std::filesystem::remove(ds_store);
                            }
                            if (std::filesystem::is_empty(parent)) {
                                std::filesystem::remove(parent);
                                UserSettingsOps::remove(*database_manager_, "ejectionPathErrored:" + history.original_location_key);
                            }
                        } catch (...) {}

                        success_count++;
                    }
                    else
                    {
                        std::filesystem::remove(dest);
                        failure_count++;
                    }
                }
                catch (const std::exception& e)
                {
                    failure_count++;
                }
            }

            Poco::JSON::Object result_obj;
            result_obj.set("success", success_count > 0 || (success_count == 0 && failure_count == 0 && histories.empty()));
            result_obj.set("success_count", success_count);
            result_obj.set("failure_count", failure_count);

            std::ostringstream oss;
            result_obj.stringify(oss);
            sendJsonResponse(response, oss.str());
        }
        catch (const std::exception &e)
        {
            sendErrorResponse(response, "Internal server error: " + std::string(e.what()), 500);
        }
    }

} // namespace MediaDedup
