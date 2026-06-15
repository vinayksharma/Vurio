#include "core/web/web_handlers_duplicates.hpp"
#include "database/database_manager.hpp"
#include "database/user_settings_ops.hpp"
#include "database/scanned_files_ops.hpp"
#include "database/duplicate_groups_ops.hpp"
#include "database/ejected_files_ops.hpp"
#include "filesmanager/media_availability.hpp"
#include "orchestration/duplicate_finder.hpp"
#include "orchestration/scheduler_service.hpp"
#include "orchestration/thread_pool_manager.hpp"
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

        static std::string calculateFileSHA1(const std::string& path) {
            Poco::SHA1Engine sha1;
            Poco::DigestOutputStream dos(sha1);
            Poco::FileInputStream fis(path);
            Poco::StreamCopier::copyStream(fis, dos);
            dos.close();
            return Poco::DigestEngine::digestToHex(sha1.digest());
        }

        // Copy src -> dest with single-pass source hashing, then verify the written
        // destination matches. See header for the full contract. The source is read
        // exactly once (instead of once for the copy and again for the hash).
        VerifiedMoveResult copyFileVerified(const std::filesystem::path& src,
                                            const std::filesystem::path& dest) {
            VerifiedMoveResult r;
            try {
                Poco::SHA1Engine src_sha;
                {
                    std::ifstream in(src, std::ios::binary);
                    std::ofstream out(dest, std::ios::binary | std::ios::trunc);
                    if (!in.is_open() || !out.is_open()) {
                        std::error_code rmec;
                        std::filesystem::remove(dest, rmec);
                        r.error = "Failed to open file streams for copy";
                        return r;
                    }
                    std::vector<char> buffer(1u << 20); // 1 MiB
                    while (in.good()) {
                        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                        std::streamsize n = in.gcount();
                        if (n > 0) {
                            out.write(buffer.data(), n);
                            src_sha.update(buffer.data(), static_cast<std::size_t>(n));
                        }
                    }
                    out.flush();
                    if (in.bad() || !out.good()) {
                        out.close();
                        std::error_code rmec;
                        std::filesystem::remove(dest, rmec);
                        r.error = "I/O error during copy";
                        return r;
                    }
                }

                std::string src_hex = Poco::DigestEngine::digestToHex(src_sha.digest());
                std::string dest_hex = calculateFileSHA1(dest.string());
                if (src_hex != dest_hex) {
                    std::error_code rmec;
                    std::filesystem::remove(dest, rmec);
                    r.error = "SHA1 mismatch after copy - file corruption prevented.";
                    return r;
                }

                // Best-effort: preserve the source's modification time on the copy
                // (rename preserves it inherently).
                try {
                    std::error_code tec;
                    std::filesystem::last_write_time(dest, std::filesystem::last_write_time(src, tec), tec);
                } catch (...) {}

                r.ok = true;
                r.wasRename = false;
                return r;
            } catch (const std::exception& e) {
                std::error_code rmec;
                std::filesystem::remove(dest, rmec);
                r.error = std::string("Exception during copy: ") + e.what();
                return r;
            }
        }

        // Move a file from src to dest while guaranteeing the destination is a
        // byte-for-byte copy of the source.
        //
        // Fast path: std::filesystem::rename. Within a single filesystem this is an
        // atomic metadata operation - no bytes are copied, so there is nothing to
        // corrupt. It is both far faster and strictly safer than copy+verify+delete,
        // and is the common case here because the ejection folder lives under the
        // media root, on the same volume as the source.
        //
        // Fallback (genuine cross-device move, EXDEV): copyFileVerified.
        VerifiedMoveResult moveFileVerified(const std::filesystem::path& src,
                                            const std::filesystem::path& dest) {
            VerifiedMoveResult r;

            std::error_code ec;
            std::filesystem::rename(src, dest, ec);
            if (!ec) {
                r.ok = true;
                r.wasRename = true;
                return r;
            }
            if (ec != std::errc::cross_device_link) {
                // A real failure (permissions, missing source, ...). Do not fall back
                // to copy - the rename was expected to succeed on the same volume.
                r.error = ec.message();
                return r;
            }

            return copyFileVerified(src, dest);
        }

        BulkOpResult bulkEject(MediaDedup::DatabaseManager& db,
                               const std::vector<std::string>& filePaths)
        {
            Poco::Logger& logger = Poco::Logger::get("EjectService");
            BulkOpResult result;
            int& success_count = result.successCount;
            int& failure_count = result.failureCount;
            int& skipped_unavailable = result.skippedUnavailable;
            std::string& last_error = result.lastError;

            // Snapshot the mount table once so we can skip files on disconnected volumes.
            auto mounted_fs = MediaDedup::MediaAvailability::mountedFilesystems();

            // DB mutations are accumulated and applied in a single transaction after
            // every on-disk move succeeds (one commit/fsync per request).
            struct EjectStaged {
                int file_id;
                std::string old_path;
                std::string new_path;
                std::string new_relative_path;
                std::string location_key;
                MediaDedup::EjectedFileHistory history;
            };
            std::vector<EjectStaged> staged;

            // On-disk moves already performed, for post-commit cleanup / rollback.
            struct AppliedMove {
                std::filesystem::path src;
                std::filesystem::path dest;
                bool wasRename;
            };
            std::vector<AppliedMove> applied;

            // Per-location settings resolved once per request, not per file.
            std::unordered_map<std::string, std::string> ejectionPathByLocation;
            std::unordered_map<std::string, std::string> mediaPathByLocation;

            for (const auto& file_path : filePaths)
            {
                if (!MediaDedup::MediaAvailability::isPathAvailable(file_path, mounted_fs)) {
                    logger.information("Skipping eject for file on disconnected volume: %s", file_path);
                    skipped_unavailable++;
                    continue;
                }

                auto file_opt = MediaDedup::ScannedFilesOps::getByPath(db, file_path);
                if (!file_opt) {
                    logger.warning("File not found in DB: %s", file_path);
                    last_error = "File not found in database: " + file_path;
                    failure_count++;
                    continue;
                }

                std::string location_key = file_opt->location_key;
                std::string ejection_path;

                if (auto it = ejectionPathByLocation.find(location_key); it != ejectionPathByLocation.end()) {
                    ejection_path = it->second;
                } else {
                    if (!MediaDedup::UserSettingsOps::get(db, "ejectionPath:" + location_key, ejection_path)) {
                        std::string media_location_path;
                        if (!MediaDedup::UserSettingsOps::get(db, location_key, media_location_path)) {
                            logger.error("Cannot find media location for key: %s", location_key);
                            last_error = "Cannot find registered media location for key: " + location_key;
                            failure_count++;
                            continue;
                        }
                        Poco::UUID uuid = Poco::UUIDGenerator::defaultGenerator().create();
                        std::filesystem::path root(media_location_path);
                        std::filesystem::path ejected = root / ("ejected_" + uuid.toString());
                        ejection_path = ejected.string();
                        std::replace(ejection_path.begin(), ejection_path.end(), '\\', '/');
                        try {
                            std::filesystem::create_directories(ejected);
                            MediaDedup::UserSettingsOps::upsert(db, "ejectionPath:" + location_key, ejection_path);
                        } catch (const std::exception& e) {
                            logger.error("Failed to create ejection directory %s: %s", ejected.string(), std::string(e.what()));
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
                                logger.error("Failed to recreate missing ejection directory %s: %s", ejection_path, std::string(e.what()));
                                last_error = "Failed to recreate missing ejection directory: " + std::string(e.what());
                                failure_count++;
                                continue;
                            }
                        }
                    }
                    ejectionPathByLocation[location_key] = ejection_path;
                }

                std::string normalized_file_path = file_path;
                std::replace(normalized_file_path.begin(), normalized_file_path.end(), '\\', '/');
                if (normalized_file_path.find(ejection_path) == 0) {
                    logger.information("File already in ejection folder: %s", file_path);
                    success_count++;
                    continue;
                }

                std::filesystem::path src(file_path);
                if (!std::filesystem::exists(src)) {
                    logger.warning("File does not exist on disk: %s", file_path);
                    last_error = "File does not exist on disk: " + file_path;
                    failure_count++;
                    continue;
                }

                std::string dest_filename = src.filename().string();
                std::filesystem::path base_eject = std::filesystem::path(ejection_path);
                std::filesystem::path dest = base_eject / dest_filename;

                int counter = 0;
                while (std::filesystem::exists(dest)) {
                    counter++;
                    std::string folder_name = dest_filename + "_" + std::to_string(counter);
                    std::filesystem::path collision_folder = base_eject / folder_name;
                    if (std::filesystem::exists(collision_folder) && !std::filesystem::is_directory(collision_folder)) {
                        continue;
                    }
                    dest = collision_folder / dest_filename;
                }

                if (dest.has_parent_path()) {
                    std::filesystem::create_directories(dest.parent_path());
                }

                VerifiedMoveResult mv = moveFileVerified(src, dest);
                if (!mv.ok) {
                    logger.error("Error ejecting file %s: %s", file_path, mv.error);
                    last_error = "Error moving file: " + mv.error;
                    failure_count++;
                    continue;
                }

                applied.push_back({src, dest, mv.wasRename});

                std::string new_file_path = dest.string();
                std::replace(new_file_path.begin(), new_file_path.end(), '\\', '/');

                std::string new_relative_path;
                std::string media_location_path;
                if (auto mit = mediaPathByLocation.find(location_key); mit != mediaPathByLocation.end()) {
                    media_location_path = mit->second;
                } else if (MediaDedup::UserSettingsOps::get(db, location_key, media_location_path)) {
                    mediaPathByLocation[location_key] = media_location_path;
                }
                if (!media_location_path.empty()) {
                    new_relative_path = std::filesystem::relative(dest, std::filesystem::path(media_location_path)).string();
                    std::replace(new_relative_path.begin(), new_relative_path.end(), '\\', '/');
                }

                EjectStaged st;
                st.file_id = file_opt->id;
                st.old_path = file_path;
                st.new_path = new_file_path;
                st.new_relative_path = new_relative_path;
                st.location_key = location_key;
                st.history.file_id = file_opt->id;
                st.history.ejected_path = new_file_path;
                st.history.original_file_path = file_opt->file_path;
                st.history.original_relative_path = file_opt->relative_path;
                st.history.original_location_key = location_key;
                staged.push_back(std::move(st));
            }

            bool committed = false;
            if (!staged.empty()) {
                try {
                    auto lease = db.acquireSessionLease();
                    Poco::Data::Session& sess = lease.get();
                    sess.begin();
                    for (const auto& st : staged) {
                        MediaDedup::EjectedFilesOps::insert(sess, st.history);
                        MediaDedup::ScannedFilesOps::updateFilePath(sess, st.file_id, st.new_path, st.new_relative_path, st.location_key);
                        MediaDedup::ScannedFilesOps::updateFilePathInImageArtifacts(sess, st.old_path, st.new_path);
                        MediaDedup::ScannedFilesOps::updateFilePathInProcessingErrors(sess, st.old_path, st.new_path);
                        MediaDedup::ScannedFilesOps::updateFilePathInDuplicateGroups(sess, st.old_path, st.new_path);
                        MediaDedup::ScannedFilesOps::updateFilePathInDuplicateMembers(sess, st.old_path, st.new_path);
                        MediaDedup::ScannedFilesOps::updateFilePathInThumbnailCache(sess, st.old_path, st.new_path);
                        MediaDedup::ScannedFilesOps::updateFilePathInPreviewCache(sess, st.old_path, st.new_path);
                    }
                    sess.commit();
                    committed = true;
                } catch (const std::exception& e) {
                    logger.error("DB commit failed during eject batch, rolling back " +
                                 std::to_string(applied.size()) + " move(s): " + std::string(e.what()));
                    for (const auto& a : applied) {
                        std::error_code rec;
                        if (a.wasRename) {
                            std::filesystem::rename(a.dest, a.src, rec);
                        } else {
                            std::filesystem::remove(a.dest, rec);
                        }
                    }
                    last_error = std::string("Database update failed: ") + e.what();
                    failure_count += static_cast<int>(staged.size());
                }
            }

            if (committed) {
                for (const auto& a : applied) {
                    if (!a.wasRename) {
                        std::error_code rec;
                        std::filesystem::remove(a.src, rec);
                    }
                }
                success_count += static_cast<int>(staged.size());
                logger.information("Ejected " + std::to_string(staged.size()) + " file(s) in batch");
            }

            return result;
        }

        BulkOpResult bulkUndoEject(MediaDedup::DatabaseManager& db,
                                   const std::vector<std::string>& filePaths,
                                   bool undoAll)
        {
            Poco::Logger& logger = Poco::Logger::get("UndoEjectService");
            BulkOpResult result;
            int& success_count = result.successCount;
            int& failure_count = result.failureCount;
            int& skipped_unavailable = result.skippedUnavailable;
            std::string& last_error = result.lastError;

            std::vector<MediaDedup::EjectedFileHistory> histories;
            auto mounted_fs = MediaDedup::MediaAvailability::mountedFilesystems();

            if (undoAll) {
                auto all = MediaDedup::EjectedFilesOps::getAll(db);
                for (const auto& h : all) {
                    if (h.ejected_path.find("/ejected_") != std::string::npos) {
                        histories.push_back(h);
                    }
                }
            } else {
                for (const auto& f : filePaths) {
                    auto hist = MediaDedup::EjectedFilesOps::getByEjectedPath(db, f);
                    if (!hist) {
                        hist = MediaDedup::EjectedFilesOps::getByOriginalPath(db, f);
                    }
                    if (!hist) {
                        // Fallback for stranded ejected files (e.g. history wiped)
                        auto user_settings = MediaDedup::UserSettingsOps::list(db);
                        for (const auto& kv : user_settings) {
                            if (kv.first.rfind("ejectionPath:", 0) == 0) {
                                std::string ep = kv.second;
                                std::replace(ep.begin(), ep.end(), '\\', '/');
                                if (f.find(ep) == 0) {
                                    std::string loc_key = kv.first.substr(13);
                                    std::string loc_path;
                                    if (MediaDedup::UserSettingsOps::get(db, loc_key, loc_path)) {
                                        std::filesystem::path ejected_f(f);
                                        std::filesystem::path original_f = std::filesystem::path(loc_path) / ejected_f.filename();
                                        MediaDedup::EjectedFileHistory fake_hist;
                                        fake_hist.file_id = -1;
                                        auto stranded_opt = MediaDedup::ScannedFilesOps::getByPath(db, f);
                                        if (stranded_opt) {
                                            fake_hist.file_id = stranded_opt->id;
                                        }
                                        fake_hist.ejected_path = f;
                                        fake_hist.original_file_path = original_f.string();
                                        std::replace(fake_hist.original_file_path.begin(), fake_hist.original_file_path.end(), '\\', '/');
                                        fake_hist.original_location_key = loc_key;
                                        fake_hist.original_relative_path = ejected_f.filename().string();
                                        hist = fake_hist;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    if (hist) {
                        histories.push_back(*hist);
                    } else {
                        logger.warning("No ejection history found for %s", f);
                        last_error = "No ejection history found for " + f;
                        failure_count++;
                    }
                }
            }

            if (histories.size() != filePaths.size() && !undoAll) {
                failure_count += static_cast<int>(filePaths.size() - histories.size());
            }

            struct UndoStaged {
                bool dest_exists;
                bool wasRename;
                int file_id;
                std::string ejected_path;
                std::string dest_path_str;
                std::string original_relative_path;
                std::string original_location_key;
                std::filesystem::path src;
                std::filesystem::path dest;
            };
            std::vector<UndoStaged> staged;

            for (const auto& history : histories) {
                std::string ejected_path = history.ejected_path;
                std::string original_path = history.original_file_path;

                if (!MediaDedup::MediaAvailability::isPathAvailable(ejected_path, mounted_fs) ||
                    !MediaDedup::MediaAvailability::isPathAvailable(original_path, mounted_fs)) {
                    logger.information("Skipping undo-eject for file on disconnected volume: %s", ejected_path);
                    skipped_unavailable++;
                    continue;
                }

                std::filesystem::path src(ejected_path);
                std::filesystem::path dest(original_path);

                if (!std::filesystem::exists(src)) {
                    logger.warning("Ejected file does not exist on disk: %s", ejected_path);
                    MediaDedup::EjectedFilesOps::deleteByEjectedPath(db, ejected_path);
                    failure_count++;
                    continue;
                }

                VerifiedMoveResult mv = moveFileVerified(src, dest);
                if (!mv.ok) {
                    logger.error("Error restoring file %s: %s", ejected_path, mv.error);
                    last_error = "Error moving file: " + mv.error;
                    failure_count++;
                    continue;
                }

                std::string dest_path_str = dest.string();
                std::replace(dest_path_str.begin(), dest_path_str.end(), '\\', '/');

                UndoStaged st;
                st.dest_exists = MediaDedup::ScannedFilesOps::getByPath(db, dest_path_str).has_value();
                st.wasRename = mv.wasRename;
                st.file_id = history.file_id;
                st.ejected_path = ejected_path;
                st.dest_path_str = dest_path_str;
                st.original_relative_path = history.original_relative_path;
                st.original_location_key = history.original_location_key;
                st.src = src;
                st.dest = dest;
                staged.push_back(std::move(st));
            }

            // dest_exists rows are removed first; completelyRemoveById manages its
            // own session, so it runs before opening the batch transaction.
            for (const auto& st : staged) {
                if (st.dest_exists && st.file_id != -1) {
                    MediaDedup::ScannedFilesOps::completelyRemoveById(db, st.file_id);
                }
            }

            bool committed = false;
            if (!staged.empty()) {
                try {
                    auto lease = db.acquireSessionLease();
                    Poco::Data::Session& sess = lease.get();
                    sess.begin();
                    for (const auto& st : staged) {
                        if (!st.dest_exists) {
                            MediaDedup::ScannedFilesOps::updateFilePath(sess, st.file_id, st.dest_path_str, st.original_relative_path, st.original_location_key);
                            MediaDedup::ScannedFilesOps::updateFilePathInImageArtifacts(sess, st.ejected_path, st.dest_path_str);
                            MediaDedup::ScannedFilesOps::updateFilePathInProcessingErrors(sess, st.ejected_path, st.dest_path_str);
                            MediaDedup::ScannedFilesOps::updateFilePathInDuplicateGroups(sess, st.ejected_path, st.dest_path_str);
                            MediaDedup::ScannedFilesOps::updateFilePathInDuplicateMembers(sess, st.ejected_path, st.dest_path_str);
                            MediaDedup::ScannedFilesOps::updateFilePathInThumbnailCache(sess, st.ejected_path, st.dest_path_str);
                            MediaDedup::ScannedFilesOps::updateFilePathInPreviewCache(sess, st.ejected_path, st.dest_path_str);
                        }
                        MediaDedup::EjectedFilesOps::deleteByEjectedPath(sess, st.ejected_path);
                    }
                    sess.commit();
                    committed = true;
                } catch (const std::exception& e) {
                    logger.error("DB commit failed during undo-eject batch, rolling back " +
                                 std::to_string(staged.size()) + " move(s): " + std::string(e.what()));
                    for (const auto& st : staged) {
                        std::error_code rec;
                        if (st.wasRename) {
                            std::filesystem::rename(st.dest, st.src, rec);
                        } else {
                            std::filesystem::remove(st.dest, rec);
                        }
                    }
                    last_error = std::string("Database update failed: ") + e.what();
                    failure_count += static_cast<int>(staged.size());
                }
            }

            if (committed) {
                success_count += static_cast<int>(staged.size());
                for (const auto& st : staged) {
                    if (!st.wasRename) {
                        std::error_code rec;
                        std::filesystem::remove(st.src, rec);
                    }
                    // Clean up empty ejection directories (best-effort).
                    try {
                        std::filesystem::path parent = st.src.parent_path();
                        std::filesystem::path ds_store = parent / ".DS_Store";
                        if (std::filesystem::exists(ds_store)) {
                            std::filesystem::remove(ds_store);
                        }
                        if (std::filesystem::is_empty(parent)) {
                            std::filesystem::remove(parent);
                            logger.information("Removed empty directory: %s", parent.string());
                            std::filesystem::path grandparent = parent.parent_path();
                            std::filesystem::path gp_ds_store = grandparent / ".DS_Store";
                            if (std::filesystem::exists(gp_ds_store)) {
                                std::filesystem::remove(gp_ds_store);
                            }
                            if (std::filesystem::exists(grandparent) && std::filesystem::is_empty(grandparent)) {
                                std::filesystem::remove(grandparent);
                                logger.information("Removed empty grandparent directory: %s", grandparent.string());
                            }
                            std::string ejection_key = "ejectionPath:" + st.original_location_key;
                            std::string registered_path;
                            if (MediaDedup::UserSettingsOps::get(db, ejection_key, registered_path)) {
                                if (!std::filesystem::exists(registered_path)) {
                                    MediaDedup::UserSettingsOps::remove(db, ejection_key);
                                    logger.information("Removed DB entry for empty ejection path: %s", ejection_key);
                                }
                            }
                        }
                    } catch (...) {
                        // Ignore directory remove errors
                    }
                }
                logger.information("Restored " + std::to_string(staged.size()) + " file(s) in batch");
            }

            return result;
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

                auto all_histories = MediaDedup::EjectedFilesOps::getAll(*database_manager_);
                int success_count = 0;
                int failure_count = 0;

                std::string purity_str = config_manager_->getPropertyValue<std::string>("duplicates.purity.order", "CAMERA_RAW, EXIF_CAPTURE_DATE, FILE_SIZE");

                for (const auto& hist : all_histories)
                {
                    if (hist.ejected_path.find("/ejected_") != std::string::npos ||
                        hist.ejected_path.find("/errored_") != std::string::npos)
                    {
                        // Check if it's a representative of any group
                        auto groups = MediaDedup::DuplicateGroupsOps::getGroupsForFile(*database_manager_, hist.file_id, "EMBEDDING");
                        for (const auto& g : groups) {
                            if (g.representative_file_id == hist.file_id) {
                                // It's a representative, promote another member!
                                MediaDedup::Orchestration::DuplicateFinder::promoteNextBestRepresentative(
                                    *database_manager_, g.id, hist.file_id, purity_str
                                );
                            }
                        }

                        // Completely wipe the file from the database
                        if (MediaDedup::ScannedFilesOps::completelyRemoveById(*database_manager_, hist.file_id)) {
                            success_count++;
                            logger.information("Successfully purged file ID %d (%s) from the database.", hist.file_id, hist.ejected_path);
                            
                            // Check the remaining size of the groups the file belonged to and sync them
                            for (const auto& g : groups) {
                                MediaDedup::DuplicateGroupsOps::syncGroupMemberCount(*database_manager_, g.id);
                            }
                        } else {
                            failure_count++;
                            logger.error("Failed to purge file ID %d from the database.", hist.file_id);
                        }
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

        IntegrityCheckState DatabaseIntegrityCheckHandler::state_;
        std::mutex DatabaseIntegrityCheckHandler::state_mutex_;

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

        bool DatabaseIntegrityCheckHandler::startIntegrityCheck(
            std::shared_ptr<MediaDedup::DatabaseManager> db,
            std::shared_ptr<MediaDedup::UnifiedObservableConfigManager> cfg,
            std::shared_ptr<MediaDedup::Orchestration::SchedulerService> scheduler,
            std::shared_ptr<MediaDedup::ThreadPoolManager> tpm,
            std::string& out_error)
        {
            Poco::Logger &logger = Poco::Logger::get("DatabaseIntegrityCheckHandler");

            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                if (state_.is_running) {
                    out_error = "An integrity check is already running";
                    return false;
                }
                
                // Reset state
                state_.is_running = true;
                state_.progress = 0;
                state_.total_files = 0;
                state_.processed_files = 0;
                state_.files_removed = 0;
                state_.groups_removed = 0;
                state_.thumbnails_removed = 0;
                state_.artifacts_removed = 0;
                state_.errors_removed = 0;
                state_.current_phase = "Initializing...";
            }

            logger.information("Starting background database integrity check.");

            // Launch detached thread
            std::thread t(performIntegrityCheck, db, cfg, scheduler, tpm);
            t.detach();

            return true;
        }

        IntegrityCheckStatusSnapshot DatabaseIntegrityCheckHandler::getIntegrityCheckStatus()
        {
            IntegrityCheckStatusSnapshot current_state;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                current_state.is_running = (bool)state_.is_running;
                current_state.progress = (int)state_.progress;
                current_state.total_files = (int)state_.total_files;
                current_state.processed_files = (int)state_.processed_files;
                current_state.files_removed = (int)state_.files_removed;
                current_state.groups_removed = (int)state_.groups_removed;
                current_state.thumbnails_removed = (int)state_.thumbnails_removed;
                current_state.artifacts_removed = (int)state_.artifacts_removed;
                current_state.errors_removed = (int)state_.errors_removed;
                current_state.current_phase = state_.current_phase;
            }
            return current_state;
        }

        void DatabaseIntegrityCheckHandler::handleStart(Poco::Net::HTTPServerResponse &response)
        {
            std::string error;
            if (!startIntegrityCheck(database_manager_, config_manager_, scheduler_service_, tpm_, error)) {
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
            IntegrityCheckStatusSnapshot current_state = getIntegrityCheckStatus();
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

        void DatabaseIntegrityCheckHandler::performIntegrityCheck(
            std::shared_ptr<MediaDedup::DatabaseManager> db,
            std::shared_ptr<MediaDedup::UnifiedObservableConfigManager> cfg,
            std::shared_ptr<MediaDedup::Orchestration::SchedulerService> scheduler,
            std::shared_ptr<MediaDedup::ThreadPoolManager> tpm)
        {
            Poco::Logger &logger = Poco::Logger::get("DatabaseIntegrityCheckTask");

            try {
                // Phase 1: Graceful Drain & Halt
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    state_.current_phase = "Halting background threads and draining queues...";
                }
                logger.information("Pausing scheduler service to prevent concurrent DB changes...");
                scheduler->pause();

                logger.information("Clearing ThreadPoolManager queues...");
                tpm->clearQueues();

                logger.information("Waiting for ThreadPoolManager to drain running tasks...");
                while (tpm->getStatus().runningTotal > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }
                logger.information("All background threads halted successfully.");

                // Phase 2: Sweep Scanned Files
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    state_.current_phase = "Checking physical file existence...";
                }
                logger.information("Fetching all scanned files...");
                auto all_files = MediaDedup::ScannedFilesOps::listAll(*db);
                
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    state_.total_files = all_files.size();
                }

                std::string purity_str = cfg->getPropertyValue<std::string>("duplicates.purity.order", "CAMERA_RAW, EXIF_CAPTURE_DATE, FILE_SIZE");

                for (size_t i = 0; i < all_files.size(); ++i) {
                    const auto& file = all_files[i];

                    if (!std::filesystem::exists(file.file_path)) {
                        logger.information("File physically missing, removing from DB: %s", file.file_path);
                        
                        // Promote if representative
                        auto groups = MediaDedup::DuplicateGroupsOps::getGroupsForFile(*db, file.id, "EMBEDDING");
                        for (const auto& g : groups) {
                            if (g.representative_file_id == file.id) {
                                MediaDedup::Orchestration::DuplicateFinder::promoteNextBestRepresentative(*db, g.id, file.id, purity_str);
                            }
                        }

                        // Completely wipe
                        if (MediaDedup::ScannedFilesOps::completelyRemoveById(*db, file.id)) {
                            std::lock_guard<std::mutex> lock(state_mutex_);
                            state_.files_removed++;
                        }

                        // Check remaining groups size and sync them
                        for (const auto& g : groups) {
                            int actual_count = MediaDedup::DuplicateGroupsOps::syncGroupMemberCount(*db, g.id);
                            if (actual_count <= 1) {
                                std::lock_guard<std::mutex> lock(state_mutex_);
                                state_.groups_removed++;
                            }
                        }
                    }

                    // Update progress periodically to avoid lock contention
                    if (i % 50 == 0) {
                        std::lock_guard<std::mutex> lock(state_mutex_);
                        state_.processed_files = i + 1;
                        state_.progress = (int)(((float)(i + 1) / all_files.size()) * 70.0f); // files check is 70% of total
                    }
                }

                // Phase 3: Cleanup Orphaned Groups
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    state_.current_phase = "Cleaning orphaned groups...";
                    state_.progress = 75;
                }
                {
                    auto lease = db->acquireSessionLease();
                    Poco::Data::Session &sess = lease.get();
                    
                    // First, fix member counts by syncing with actual distinct files
                    Poco::Data::Statement update_counts(sess);
                    update_counts << "UPDATE duplicate_groups SET member_count = ("
                                     "  SELECT COUNT(DISTINCT file_id) FROM ("
                                     "    SELECT file_id FROM duplicate_members dm WHERE dm.group_id = duplicate_groups.id "
                                     "    UNION "
                                     "    SELECT representative_file_id FROM duplicate_groups dg WHERE dg.id = duplicate_groups.id"
                                     "  )"
                                     ") WHERE member_count != ("
                                     "  SELECT COUNT(DISTINCT file_id) FROM ("
                                     "    SELECT file_id FROM duplicate_members dm WHERE dm.group_id = duplicate_groups.id "
                                     "    UNION "
                                     "    SELECT representative_file_id FROM duplicate_groups dg WHERE dg.id = duplicate_groups.id"
                                     "  )"
                                     ")";
                    int synced = update_counts.execute();
                    if (synced > 0) {
                        logger.information("Synchronized member_count for %d duplicate groups.", synced);
                    }

                    // Delete groups with 0 or 1 members left
                    Poco::Data::Statement del_groups(sess);
                    del_groups << "DELETE FROM duplicate_groups WHERE member_count <= 1";
                    int removed = del_groups.execute();
                    if (removed > 0) {
                        std::lock_guard<std::mutex> lock(state_mutex_);
                        state_.groups_removed += removed;
                        logger.information("Removed %d orphaned duplicate groups.", removed);
                    }
                }

                // Phase 4: Cleanup Metadata (Thumbnails, Artifacts, Errors)
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    state_.current_phase = "Cleaning orphaned metadata...";
                    state_.progress = 85;
                }
                {
                    auto lease = db->acquireSessionLease();
                    Poco::Data::Session &sess = lease.get();

                    // For thumbnails, we should delete the physical file too
                    Poco::Data::Statement sel_thumbs(sess);
                    sel_thumbs << "SELECT cached_path FROM thumbnail_cache WHERE source_path NOT IN (SELECT file_path FROM scanned_files)";
                    Poco::Data::RecordSet rs(sel_thumbs);
                    int thumbs_removed = 0;
                    bool more = rs.moveFirst();
                    while (more) {
                        std::string cached_path = rs[0].convert<std::string>();
                        try {
                            if (std::filesystem::exists(cached_path)) {
                                std::filesystem::remove(cached_path);
                            }
                        } catch (...) {}
                        thumbs_removed++;
                        more = rs.moveNext();
                    }

                    Poco::Data::Statement del_thumbs(sess);
                    del_thumbs << "DELETE FROM thumbnail_cache WHERE source_path NOT IN (SELECT file_path FROM scanned_files)";
                    del_thumbs.execute();
                    
                    Poco::Data::Statement del_artifacts(sess);
                    del_artifacts << "DELETE FROM image_artifacts WHERE file_path NOT IN (SELECT file_path FROM scanned_files)";
                    int artifacts_removed = del_artifacts.execute();

                    Poco::Data::Statement del_errors(sess);
                    del_errors << "DELETE FROM processing_errors WHERE file_path NOT IN (SELECT file_path FROM scanned_files)";
                    int errors_removed = del_errors.execute();

                    Poco::Data::Statement del_resolved_errors(sess);
                    del_resolved_errors << "DELETE FROM processing_errors WHERE file_path IN (SELECT file_path FROM scanned_files WHERE processed >= 0)";
                    int resolved_errors_removed = del_resolved_errors.execute();
                    errors_removed += resolved_errors_removed;

                    {
                        std::lock_guard<std::mutex> lock(state_mutex_);
                        state_.thumbnails_removed += thumbs_removed;
                        state_.artifacts_removed += artifacts_removed;
                        state_.errors_removed += errors_removed;
                    }
                    
                    if (thumbs_removed > 0) logger.information("Removed %d orphaned thumbnails.", thumbs_removed);
                    if (artifacts_removed > 0) logger.information("Removed %d orphaned artifacts.", artifacts_removed);
                    if (errors_removed > 0) logger.information("Removed %d orphaned errors.", errors_removed);
                }

                // Phase 5: Reclaim Database Space
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    state_.current_phase = "Reclaiming database space (VACUUM)...";
                    state_.progress = 95;
                }
                {
                    try {
                        auto lease = db->acquireSessionLease();
                        Poco::Data::Session &sess = lease.get();
                        Poco::Data::Statement vac(sess);
                        vac << "VACUUM";
                        vac.execute();
                        logger.information("Database vacuumed successfully to reclaim space.");
                    } catch (const std::exception& e) {
                        logger.warning("Database vacuum failed: %s", std::string(e.what()));
                    }
                }

                // Finish
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    state_.current_phase = "Resuming background services...";
                    state_.progress = 100;
                }

                logger.information("Resuming scheduler service...");
                scheduler->resume();

                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    state_.current_phase = "Complete";
                    state_.is_running = false;
                }

                logger.information("Database Integrity Check completed successfully.");

            } catch (const std::exception &e) {
                logger.error("Exception during database integrity check: %s", std::string(e.what()));
                
                // Attempt to resume scheduler on failure
                try {
                    scheduler->resume();
                } catch(...) {}

                std::lock_guard<std::mutex> lock(state_mutex_);
                state_.is_running = false;
                state_.current_phase = "Failed: " + std::string(e.what());
            }
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
