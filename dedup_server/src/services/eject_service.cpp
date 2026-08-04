#include "services/eject_service.hpp"

#include "database/database_manager.hpp"
#include "database/duplicate_groups_ops.hpp"
#include "database/ejected_files_ops.hpp"
#include "database/scanned_files_ops.hpp"
#include "database/user_settings_ops.hpp"
#include "filesmanager/media_availability.hpp"
#include "orchestration/duplicate_finder.hpp"
#include "config/unified_observable_config.hpp"

#include <Poco/DigestEngine.h>
#include <Poco/DigestStream.h>
#include <Poco/FileStream.h>
#include <Poco/Logger.h>
#include <Poco/SHA1Engine.h>
#include <Poco/StreamCopier.h>
#include <Poco/UUIDGenerator.h>
#include <Poco/Data/Session.h>
#include <Poco/Data/Statement.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <unordered_map>

namespace MediaDedup
{
    namespace Services
    {
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


        // -----------------------------------------------------------------
        // AR-2: category ejects (errored / ignored). This is the HTTP fork —
        // the correct one: collision-safe destination folders, SHA1-verified
        // copies, transactional DB updates that roll back the copied file on
        // failure, pre-existing-row and eject-history cleanup. The gRPC
        // clones (which lacked all four) are deleted; both surfaces call
        // these.
        // -----------------------------------------------------------------
        namespace
        {
            struct CategorySpec {
                std::string settings_key_prefix; // "ejectionPathErrored:" / "ejectionPathIgnored:"
                std::string dir_prefix;          // "errored_" / "ignored_"
            };

            BulkOpResult ejectCategory(MediaDedup::DatabaseManager& db,
                                       const std::vector<MediaDedup::ScannedFileRow>& files,
                                       const CategorySpec& spec)
            {
                BulkOpResult r;
                for (const auto& file_opt : files)
                {
                    std::string file_path = file_opt.file_path;
                    std::string location_key = file_opt.location_key;
                    std::string ejection_path;

                    if (!MediaDedup::UserSettingsOps::get(db, spec.settings_key_prefix + location_key, ejection_path)) {
                        std::string media_location_path;
                        if (!MediaDedup::UserSettingsOps::get(db, location_key, media_location_path)) {
                            r.lastError = "Cannot find registered media location for key: " + location_key;
                            r.failureCount++;
                            continue;
                        }
                        Poco::UUID uuid = Poco::UUIDGenerator::defaultGenerator().create();
                        std::filesystem::path root(media_location_path);
                        std::filesystem::path ejected = root / (spec.dir_prefix + uuid.toString());
                        ejection_path = ejected.string();
                        std::replace(ejection_path.begin(), ejection_path.end(), '\\', '/');

                        try {
                            std::filesystem::create_directories(ejected);
                            MediaDedup::UserSettingsOps::upsert(db, spec.settings_key_prefix + location_key, ejection_path);
                        } catch (const std::exception& e) {
                            r.lastError = "Failed to create ejection directory: " + std::string(e.what());
                            r.failureCount++;
                            continue;
                        }
                    } else {
                        std::filesystem::path ejected(ejection_path);
                        if (!std::filesystem::exists(ejected)) {
                            try {
                                std::filesystem::create_directories(ejected);
                            } catch (const std::exception& e) {
                                r.lastError = "Failed to recreate missing ejection directory: " + std::string(e.what());
                                r.failureCount++;
                                continue;
                            }
                        }
                    }

                    std::string normalized_file_path = file_path;
                    std::replace(normalized_file_path.begin(), normalized_file_path.end(), '\\', '/');
                    if (normalized_file_path.find(ejection_path) == 0) {
                        r.successCount++;
                        continue;
                    }

                    std::filesystem::path src(file_path);
                    if (!std::filesystem::exists(src)) {
                        r.lastError = "File does not exist on disk: " + file_path;
                        r.failureCount++;
                        continue;
                    }

                    std::string dest_filename = src.filename().string();
                    std::filesystem::path base_eject = std::filesystem::path(ejection_path);
                    std::filesystem::path dest = base_eject / dest_filename;

                    // Collision-safe: name_1/, name_2/ subfolders (the gRPC
                    // fork overwrote — a second same-named file destroyed the
                    // first).
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

                    try {
                        std::filesystem::copy_file(src, dest, std::filesystem::copy_options::overwrite_existing);
                        try {
                            std::error_code ec;
                            std::filesystem::last_write_time(dest, std::filesystem::last_write_time(src, ec), ec);
                        } catch (...) {}

                        std::string src_sha1 = calculateFileSHA1(src.string());
                        std::string dest_sha1 = calculateFileSHA1(dest.string());

                        if (src_sha1 == dest_sha1) {
                            std::string new_file_path = dest.string();
                            std::replace(new_file_path.begin(), new_file_path.end(), '\\', '/');

                            std::string new_relative_path;
                            std::string media_location_path;
                            if (MediaDedup::UserSettingsOps::get(db, location_key, media_location_path)) {
                                new_relative_path = std::filesystem::relative(dest, std::filesystem::path(media_location_path)).string();
                                std::replace(new_relative_path.begin(), new_relative_path.end(), '\\', '/');
                            }

                            MediaDedup::EjectedFileHistory history;
                            history.file_id = file_opt.id;
                            history.ejected_path = new_file_path;
                            history.original_file_path = file_opt.file_path;
                            history.original_relative_path = file_opt.relative_path;
                            history.original_location_key = location_key;

                            auto existing = MediaDedup::ScannedFilesOps::getByPath(db, new_file_path);
                            if (existing) {
                                MediaDedup::ScannedFilesOps::completelyRemoveById(db, existing->id);
                            }

                            try {
                                auto lease = db.acquireSessionLease();
                                Poco::Data::Session& sess = lease.get();
                                sess.begin();

                                MediaDedup::EjectedFilesOps::deleteByEjectedPath(sess, new_file_path);
                                if (!MediaDedup::EjectedFilesOps::insert(sess, history)) {
                                    throw std::runtime_error("Failed to insert ejected file history");
                                }
                                if (!MediaDedup::ScannedFilesOps::updateFilePath(sess, file_opt.id, new_file_path, new_relative_path, location_key)) {
                                    throw std::runtime_error("Failed to update file path");
                                }
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

                            std::filesystem::remove(src);
                            r.successCount++;
                        } else {
                            std::filesystem::remove(dest);
                            r.lastError = "SHA1 mismatch after copy - file corruption prevented.";
                            r.failureCount++;
                        }
                    } catch (const std::exception& e) {
                        r.lastError = "Error copying file: " + std::string(e.what());
                        r.failureCount++;
                    }
                }
                return r;
            }

            BulkOpResult undoEjectCategory(MediaDedup::DatabaseManager& db,
                                           const CategorySpec& spec,
                                           bool clear_processing_errors)
            {
                BulkOpResult r;
                auto all_histories = MediaDedup::EjectedFilesOps::getAll(db);
                std::vector<MediaDedup::EjectedFileHistory> histories;
                const std::string needle = "/" + spec.dir_prefix;
                for (const auto& hist : all_histories) {
                    if (hist.ejected_path.find(needle) != std::string::npos) {
                        histories.push_back(hist);
                    }
                }

                for (const auto& history : histories)
                {
                    std::string ejected_path = history.ejected_path;
                    std::string original_path = history.original_file_path;

                    std::filesystem::path src(ejected_path);
                    std::filesystem::path dest(original_path);

                    if (!std::filesystem::exists(src))
                    {
                        MediaDedup::EjectedFilesOps::deleteByEjectedPath(db, ejected_path);
                        r.failureCount++;
                        continue;
                    }

                    try
                    {
                        std::filesystem::copy_file(src, dest, std::filesystem::copy_options::overwrite_existing);
                        try {
                            std::error_code ec;
                            std::filesystem::last_write_time(dest, std::filesystem::last_write_time(src, ec), ec);
                        } catch (...) {}

                        std::string src_sha1 = calculateFileSHA1(src.string());
                        std::string dest_sha1 = calculateFileSHA1(dest.string());

                        if (src_sha1 == dest_sha1)
                        {
                            std::string dest_path_str = dest.string();
                            std::replace(dest_path_str.begin(), dest_path_str.end(), '\\', '/');

                            try {
                                auto lease = db.acquireSessionLease();
                                Poco::Data::Session& sess = lease.get();
                                sess.begin();

                                if (!MediaDedup::ScannedFilesOps::updateFilePath(sess, history.file_id, dest_path_str, history.original_relative_path, history.original_location_key)) {
                                    throw std::runtime_error("Failed to update file path during undo eject");
                                }
                                MediaDedup::ScannedFilesOps::updateFilePathInImageArtifacts(sess, ejected_path, dest_path_str);
                                MediaDedup::ScannedFilesOps::updateFilePathInProcessingErrors(sess, ejected_path, dest_path_str);
                                MediaDedup::ScannedFilesOps::updateFilePathInDuplicateGroups(sess, ejected_path, dest_path_str);
                                MediaDedup::ScannedFilesOps::updateFilePathInDuplicateMembers(sess, ejected_path, dest_path_str);
                                MediaDedup::ScannedFilesOps::updateFilePathInThumbnailCache(sess, ejected_path, dest_path_str);

                                // Restored files go back to unprocessed so
                                // they are evaluated again (and errored ones
                                // additionally shed their processing_errors —
                                // "treat as if the errors were just cleared").
                                std::string dest_p = dest_path_str;
                                Poco::Data::Statement stmt_update_state(sess);
                                int unproc = 0;
                                stmt_update_state << "UPDATE scanned_files SET processed=? WHERE file_path=?", Poco::Data::Keywords::use(unproc), Poco::Data::Keywords::use(dest_p), Poco::Data::Keywords::now;

                                if (clear_processing_errors) {
                                    Poco::Data::Statement stmt_del_err(sess);
                                    stmt_del_err << "DELETE FROM processing_errors WHERE file_path=?", Poco::Data::Keywords::use(dest_p), Poco::Data::Keywords::now;
                                }

                                MediaDedup::EjectedFilesOps::deleteByEjectedPath(sess, ejected_path);

                                sess.commit();
                            } catch (...) {
                                std::filesystem::remove(dest);
                                throw;
                            }

                            std::filesystem::remove(src);

                            // Tidy the now-possibly-empty ejection folders and
                            // drop the settings key when the root is gone.
                            try {
                                std::filesystem::path parent = src.parent_path();
                                std::filesystem::path ds_store = parent / ".DS_Store";
                                if (std::filesystem::exists(ds_store)) {
                                    std::filesystem::remove(ds_store);
                                }
                                if (std::filesystem::is_empty(parent)) {
                                    std::filesystem::remove(parent);

                                    std::filesystem::path grandparent = parent.parent_path();
                                    std::filesystem::path gp_ds_store = grandparent / ".DS_Store";
                                    if (std::filesystem::exists(gp_ds_store)) {
                                        std::filesystem::remove(gp_ds_store);
                                    }
                                    if (std::filesystem::exists(grandparent) && std::filesystem::is_empty(grandparent)) {
                                        std::filesystem::remove(grandparent);
                                    }

                                    std::string ejection_key = spec.settings_key_prefix + history.original_location_key;
                                    std::string registered_path;
                                    if (MediaDedup::UserSettingsOps::get(db, ejection_key, registered_path)) {
                                        if (!std::filesystem::exists(registered_path)) {
                                            MediaDedup::UserSettingsOps::remove(db, ejection_key);
                                        }
                                    }
                                }
                            } catch (...) {}

                            r.successCount++;
                        }
                        else
                        {
                            std::filesystem::remove(dest);
                            r.failureCount++;
                        }
                    }
                    catch (const std::exception&)
                    {
                        r.failureCount++;
                    }
                }
                return r;
            }
        }

        BulkOpResult ejectErrored(MediaDedup::DatabaseManager& db)
        {
            return ejectCategory(db, MediaDedup::ScannedFilesOps::listErroredFiles(db),
                                 {"ejectionPathErrored:", "errored_"});
        }

        BulkOpResult undoEjectErrored(MediaDedup::DatabaseManager& db)
        {
            return undoEjectCategory(db, {"ejectionPathErrored:", "errored_"},
                                     /*clear_processing_errors=*/true);
        }

        BulkOpResult ejectIgnored(MediaDedup::DatabaseManager& db)
        {
            return ejectCategory(db, MediaDedup::ScannedFilesOps::listIgnoredFiles(db),
                                 {"ejectionPathIgnored:", "ignored_"});
        }

        BulkOpResult undoEjectIgnored(MediaDedup::DatabaseManager& db)
        {
            return undoEjectCategory(db, {"ejectionPathIgnored:", "ignored_"},
                                     /*clear_processing_errors=*/false);
        }

        BulkOpResult purgeEjected(MediaDedup::DatabaseManager& db,
                                  std::shared_ptr<MediaDedup::UnifiedObservableConfigManager> cfg)
        {
            // Merged from the drifted HTTP/gRPC pair: the gRPC copy also
            // purged /ignored_ histories (kept — it is the complete set);
            // the HTTP copy synced group member counts afterwards (kept —
            // it is the correctness fix).
            BulkOpResult r;
            auto all_histories = MediaDedup::EjectedFilesOps::getAll(db);

            std::string purity_str = cfg
                ? cfg->getPropertyValue<std::string>("duplicates.purity.order", "CAMERA_RAW, EXIF_CAPTURE_DATE, FILE_SIZE")
                : "CAMERA_RAW, EXIF_CAPTURE_DATE, FILE_SIZE";

            for (const auto& hist : all_histories)
            {
                if (hist.ejected_path.find("/ejected_") != std::string::npos ||
                    hist.ejected_path.find("/errored_") != std::string::npos ||
                    hist.ejected_path.find("/ignored_") != std::string::npos)
                {
                    auto groups = MediaDedup::DuplicateGroupsOps::getGroupsForFile(db, hist.file_id, "EMBEDDING");
                    for (const auto& g : groups) {
                        if (g.representative_file_id == hist.file_id) {
                            MediaDedup::Orchestration::DuplicateFinder::promoteNextBestRepresentative(
                                db, g.id, hist.file_id, purity_str);
                        }
                    }

                    if (MediaDedup::ScannedFilesOps::completelyRemoveById(db, hist.file_id)) {
                        r.successCount++;
                        for (const auto& g : groups) {
                            MediaDedup::DuplicateGroupsOps::syncGroupMemberCount(db, g.id);
                        }
                    } else {
                        r.failureCount++;
                    }
                }
            }
            return r;
        }

    } // namespace Services
} // namespace MediaDedup
