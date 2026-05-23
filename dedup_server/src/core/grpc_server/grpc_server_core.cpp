#include "core/grpc_server/grpc_server_core.hpp"
#include <grpcpp/grpcpp.h>
#include <Poco/Logger.h>
#include <filesystem>
#include "dedup.grpc.pb.h"
#include "config/unified_observable_config.hpp"
#include "database/database_manager.hpp"
#include "orchestration/files_manager.hpp"
#include <nlohmann/json.hpp>
#include "database/scanned_files_ops.hpp"
#include "database/duplicate_groups_ops.hpp"
#include "database/user_settings_ops.hpp"
#include "database/ejected_files_ops.hpp"
#include "orchestration/thread_pool_manager.hpp"
#include <Poco/SHA1Engine.h>
#include <Poco/DigestEngine.h>
#include <Poco/DigestStream.h>
#include <Poco/FileStream.h>
#include <Poco/StreamCopier.h>
#include <Poco/UUIDGenerator.h>
#include <algorithm>
#include "orchestration/duplicate_finder.hpp"
#include "filesmanager/disk_cache.hpp"

namespace MediaDedup {
namespace Core {

class ConfigServiceImpl final : public vurio::dedup::ConfigService::Service {
    std::shared_ptr<UnifiedObservableConfigManager> config_;
public:
    explicit ConfigServiceImpl(std::shared_ptr<UnifiedObservableConfigManager> c) : config_(std::move(c)) {}
    grpc::Status GetConfig(grpc::ServerContext* context, const vurio::dedup::GetConfigRequest* request,
                           vurio::dedup::GetConfigResponse* reply) override {
        nlohmann::json json_config;
        for (const auto& key : config_->getAllPropertyKeys()) {
            auto property = config_->getProperty<int>(key); // Template parameter is unused in getProperty
            if (property) {
                nlohmann::json prop_json;
                prop_json["value"] = property->getValueAsString();
                prop_json["description"] = property->getDescription();
                prop_json["longDescription"] = property->getLongDescription();
                prop_json["options"] = property->getOptions();
                json_config[key] = prop_json;
            }
        }
        reply->set_json_payload(json_config.dump(4));
        return grpc::Status::OK;
    }

    grpc::Status UpdateConfig(grpc::ServerContext* context, const vurio::dedup::UpdateConfigRequest* request,
                              vurio::dedup::UpdateConfigResponse* reply) override {
        const std::string& key = request->key();
        const std::string& value_json = request->value_json();
        
        bool success = false;
        auto property = config_->getProperty<int>(key); // Template parameter is unused in getProperty
        if (property) {
            success = property->setValueFromString(value_json);
        }
        
        reply->set_success(success);
        if (success) {
            config_->triggerSave();
        } else {
            reply->set_error_message("Failed to parse or set configuration value for key: " + key);
        }
        
        return grpc::Status::OK;
    }
};

class MediaLocationsServiceImpl final : public vurio::dedup::MediaLocationsService::Service {
    std::shared_ptr<FilesService> files_;
    std::shared_ptr<ThreadPoolManager> tpm_;
    DatabaseManager* db_;
public:
    explicit MediaLocationsServiceImpl(std::shared_ptr<FilesService> f, std::shared_ptr<ThreadPoolManager> tpm, DatabaseManager* db) 
        : files_(std::move(f)), tpm_(std::move(tpm)), db_(db) {}

    grpc::Status GetStatus(grpc::ServerContext* context, const vurio::dedup::GetStatusRequest* request,
                           vurio::dedup::GetStatusResponse* reply) override {
        auto locations = files_->listMediaLocations();
        for (const auto& pair : locations) {
            reply->add_registered_dirs(pair.second);
        }
        
        if (db_) {
            reply->set_scanned_files_count(MediaDedup::ScannedFilesOps::count(*db_));
            reply->set_processed_files_count(MediaDedup::ScannedFilesOps::countProcessed(*db_));
            reply->set_error_files_count(MediaDedup::ScannedFilesOps::countError(*db_));
            auto stats = MediaDedup::DuplicateGroupsOps::getStats(*db_, "EMBEDDING");
            reply->set_duplicate_groups_count(stats.total_groups);
            reply->set_duplicate_files_count(stats.total_duplicates);
            
            auto checkpoint = MediaDedup::DuplicateGroupsOps::getCheckpoint(*db_, "EMBEDDING");
            int last_processed_id = checkpoint.has_value() ? checkpoint->last_processed_id : 0;
            reply->set_duplicate_finding_total(reply->processed_files_count());
            reply->set_duplicate_finding_completed(MediaDedup::ScannedFilesOps::countProcessedUpTo(*db_, last_processed_id));
            
            auto user_settings = MediaDedup::UserSettingsOps::list(*db_);
            for (const auto& kv : user_settings) {
                if (kv.first.rfind("ejectionPath:", 0) == 0) {
                    reply->add_ejected_dirs(kv.second);
                }
            }
        }
        if (tpm_) {
            auto tpm_status = tpm_->getStatus();
            reply->set_thread_count(tpm_status.runningTotal);
            reply->set_max_threads(tpm_status.effectiveMax);
        }
        
        return grpc::Status::OK;
    }

    grpc::Status RegisterLocation(grpc::ServerContext* context, const vurio::dedup::RegisterLocationRequest* request,
                                  vurio::dedup::RegisterLocationResponse* reply) override {
        bool success = files_->registerMediaLocation(request->path());
        reply->set_success(success);
        if (!success) {
            reply->set_error_message("Failed to register location.");
        }
        return grpc::Status::OK;
    }

    grpc::Status DeregisterLocation(grpc::ServerContext* context, const vurio::dedup::DeregisterLocationRequest* request,
                                    vurio::dedup::DeregisterLocationResponse* reply) override {
        bool success = files_->deregisterMediaLocation(request->path());
        reply->set_success(success);
        if (!success) {
            reply->set_error_message("Failed to deregister location.");
        }
        return grpc::Status::OK;
    }

    grpc::Status ChangePath(grpc::ServerContext* context, const vurio::dedup::ChangePathRequest* request,
                            vurio::dedup::ChangePathResponse* reply) override {
        auto result = files_->changeMediaLocationPath(request->old_path(), request->new_path());
        reply->set_success(result.success);
        reply->set_error_message(result.error_message);
        reply->set_files_updated(result.files_updated);
        return grpc::Status::OK;
    }
};

class DuplicatesServiceImpl final : public vurio::dedup::DuplicatesService::Service {
    DatabaseManager* db_;

    struct StreamQueue {
        std::mutex mutex;
        std::condition_variable cv;
        std::queue<vurio::dedup::DuplicateGroupUpdate> updates;
        bool active = true;
    };
    std::mutex streams_mutex_;
    std::vector<std::shared_ptr<StreamQueue>> streams_;

    static std::string calculateFileSHA1(const std::string& path) {
        Poco::SHA1Engine sha1;
        Poco::DigestOutputStream dos(sha1);
        Poco::FileInputStream fis(path);
        Poco::StreamCopier::copyStream(fis, dos);
        dos.close();
        return Poco::DigestEngine::digestToHex(sha1.digest());
    }

public:
    explicit DuplicatesServiceImpl(DatabaseManager* db) : db_(db) {}

    void OnDuplicateGroupEvent(int group_id, bool is_new) {
        if (!db_) return;
        
        auto group_opt = MediaDedup::DuplicateGroupsOps::getGroupById(*db_, group_id);
        if (!group_opt.has_value()) return;
        
        int index = MediaDedup::DuplicateGroupsOps::getDuplicateGroupsCountOlderThan(*db_, "EMBEDDING", group_opt->representative_created_date);

        vurio::dedup::DuplicateGroupUpdate update;
        update.set_type(is_new ? vurio::dedup::DuplicateGroupUpdate::ADDED : vurio::dedup::DuplicateGroupUpdate::UPDATED);
        update.set_index(index);
        
        auto* proto_group = update.mutable_group();
        proto_group->set_group_id(std::to_string(group_id));
        
        auto members = MediaDedup::DuplicateGroupsOps::getMembersByGroup(*db_, group_id);
        
        auto user_settings = MediaDedup::UserSettingsOps::list(*db_);
        std::vector<std::string> ejection_paths;
        for (const auto& kv : user_settings) {
            if (kv.first.rfind("ejectionPath:", 0) == 0) {
                std::string ep = kv.second;
                std::replace(ep.begin(), ep.end(), '\\', '/');
                ejection_paths.push_back(ep);
            }
        }

        for (const auto& member : members) {
            auto* proto_file = proto_group->add_files();
            proto_file->set_file_path(member.file_path);
            proto_file->set_size_bytes(member.file_size);
            proto_file->set_is_representative(member.is_representative);
            proto_file->set_similarity_score(member.similarity_score);
            
            bool is_ejected = false;
            std::string normalized_path = member.file_path;
            std::replace(normalized_path.begin(), normalized_path.end(), '\\', '/');
            for (const auto& ep : ejection_paths) {
                if (normalized_path.find(ep) == 0) {
                    is_ejected = true;
                    break;
                }
            }
            proto_file->set_is_ejected(is_ejected);
        }

        std::lock_guard<std::mutex> lock(streams_mutex_);
        for (auto& queue : streams_) {
            std::lock_guard<std::mutex> q_lock(queue->mutex);
            queue->updates.push(update);
            queue->cv.notify_one();
        }
    }

    grpc::Status GetDuplicateGroups(grpc::ServerContext* context, const vurio::dedup::GetGroupsRequest* request,
                                    vurio::dedup::GetGroupsResponse* reply) override {
        if (!db_) return grpc::Status(grpc::StatusCode::INTERNAL, "Database missing");
        
        int limit = request->limit() > 0 ? request->limit() : 100;
        int offset = request->offset() >= 0 ? request->offset() : 0;
        
        auto page = MediaDedup::DuplicateGroupsOps::getGroupsWithMembers(*db_, offset, limit, "");
        reply->set_total_groups(page.total_count);
        
        auto user_settings = MediaDedup::UserSettingsOps::list(*db_);
        std::vector<std::string> ejection_paths;
        for (const auto& kv : user_settings) {
            if (kv.first.rfind("ejectionPath:", 0) == 0) {
                std::string ep = kv.second;
                std::replace(ep.begin(), ep.end(), '\\', '/');
                ejection_paths.push_back(ep);
            }
        }
        
        for (const auto& group_with_members : page.groups) {
            if (group_with_members.group.member_count < 2) continue;
            
            auto* proto_group = reply->add_groups();
            proto_group->set_group_id(std::to_string(group_with_members.group.id));
            
            for (const auto& member : group_with_members.members) {
                auto* proto_file = proto_group->add_files();
                proto_file->set_file_path(member.file_path);
                proto_file->set_size_bytes(member.file_size);
                proto_file->set_is_representative(member.is_representative);
                proto_file->set_similarity_score(member.similarity_score);
                
                bool is_ejected = false;
                std::string normalized_path = member.file_path;
                std::replace(normalized_path.begin(), normalized_path.end(), '\\', '/');
                for (const auto& ep : ejection_paths) {
                    if (normalized_path.find(ep) == 0) {
                        is_ejected = true;
                        break;
                    }
                }
                proto_file->set_is_ejected(is_ejected);
            }
        }
        return grpc::Status::OK;
    }

    grpc::Status EjectDuplicates(grpc::ServerContext* context, const vurio::dedup::EjectDuplicatesRequest* request,
                                 vurio::dedup::EjectDuplicatesResponse* reply) override {
        int success_count = 0;
        int failure_count = 0;
        std::string last_error = "";
        
        for (const auto& file_path : request->file_paths()) {
            auto file_opt = MediaDedup::ScannedFilesOps::getByPath(*db_, file_path);
            if (!file_opt) {
                last_error = "File not found in database: " + file_path;
                failure_count++;
                continue;
            }
            
            std::string location_key = file_opt->location_key;
            std::string ejection_path;
            
            if (!MediaDedup::UserSettingsOps::get(*db_, "ejectionPath:" + location_key, ejection_path)) {
                std::string media_location_path;
                if (!MediaDedup::UserSettingsOps::get(*db_, location_key, media_location_path)) {
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
                    MediaDedup::UserSettingsOps::upsert(*db_, "ejectionPath:" + location_key, ejection_path);
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
                    } catch (...) {
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
                    if (MediaDedup::UserSettingsOps::get(*db_, location_key, media_location_path)) {
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
                        auto lease = db_->acquireSessionLease();
                        Poco::Data::Session& sess = lease.get();
                        sess.begin();
                        
                        MediaDedup::EjectedFilesOps::insert(sess, history);
                        MediaDedup::ScannedFilesOps::updateFilePath(sess, file_opt->id, new_file_path, new_relative_path, location_key);
                        MediaDedup::ScannedFilesOps::updateFilePathInImageArtifacts(sess, file_path, new_file_path);
                        MediaDedup::ScannedFilesOps::updateFilePathInProcessingErrors(sess, file_path, new_file_path);
                        MediaDedup::ScannedFilesOps::updateFilePathInDuplicateGroups(sess, file_path, new_file_path);
                        MediaDedup::ScannedFilesOps::updateFilePathInDuplicateMembers(sess, file_path, new_file_path);
                        MediaDedup::ScannedFilesOps::updateFilePathInThumbnailCache(sess, file_path, new_file_path);
                        MediaDedup::ScannedFilesOps::updateFilePathInPreviewCache(sess, file_path, new_file_path);
                        
                        sess.commit();
                    } catch (...) {
                        std::filesystem::remove(dest);
                        throw;
                    }
                    std::filesystem::remove(src);
                    success_count++;
                } else {
                    std::filesystem::remove(dest);
                    failure_count++;
                }
            } catch (const std::exception& e) {
                last_error = std::string(e.what());
                failure_count++;
            }
        }
        
        reply->set_success(success_count > 0);
        reply->set_success_count(success_count);
        reply->set_failure_count(failure_count);
        reply->set_error_message(last_error);
        return grpc::Status::OK;
    }

    grpc::Status UndoEjectDuplicates(grpc::ServerContext* context, const vurio::dedup::UndoEjectDuplicatesRequest* request,
                                     vurio::dedup::UndoEjectDuplicatesResponse* reply) override {
        bool undo_all = request->undo_all();
        std::vector<MediaDedup::EjectedFileHistory> histories;
        
        if (undo_all) {
            auto all = MediaDedup::EjectedFilesOps::getAll(*db_);
            for (const auto& h : all) {
                if (h.ejected_path.find("_ejected") != std::string::npos) {
                    histories.push_back(h);
                }
            }
        } else {
            for (const auto& f : request->file_paths()) {
                auto hist = MediaDedup::EjectedFilesOps::getByEjectedPath(*db_, f);
                if (!hist) hist = MediaDedup::EjectedFilesOps::getByOriginalPath(*db_, f);
                if (hist) histories.push_back(*hist);
            }
        }
        
        int success_count = 0;
        int failure_count = 0;
        
        for (const auto& history : histories) {
            std::string ejected_path = history.ejected_path;
            std::string original_path = history.original_file_path;
            
            std::filesystem::path src(ejected_path);
            std::filesystem::path dest(original_path);
            
            if (!std::filesystem::exists(src)) {
                MediaDedup::EjectedFilesOps::deleteByEjectedPath(*db_, ejected_path);
                failure_count++;
                continue;
            }
            
            try {
                std::filesystem::copy_file(src, dest, std::filesystem::copy_options::overwrite_existing);
                std::string src_sha1 = calculateFileSHA1(src.string());
                std::string dest_sha1 = calculateFileSHA1(dest.string());
                
                if (src_sha1 == dest_sha1) {
                    std::string dest_path_str = dest.string();
                    std::replace(dest_path_str.begin(), dest_path_str.end(), '\\', '/');
                    
                    try {
                        auto lease = db_->acquireSessionLease();
                        Poco::Data::Session& sess = lease.get();
                        sess.begin();
                        
                        MediaDedup::ScannedFilesOps::updateFilePath(sess, history.file_id, dest_path_str, history.original_relative_path, history.original_location_key);
                        MediaDedup::ScannedFilesOps::updateFilePathInImageArtifacts(sess, ejected_path, dest_path_str);
                        MediaDedup::ScannedFilesOps::updateFilePathInProcessingErrors(sess, ejected_path, dest_path_str);
                        MediaDedup::ScannedFilesOps::updateFilePathInDuplicateGroups(sess, ejected_path, dest_path_str);
                        MediaDedup::ScannedFilesOps::updateFilePathInDuplicateMembers(sess, ejected_path, dest_path_str);
                        MediaDedup::ScannedFilesOps::updateFilePathInThumbnailCache(sess, ejected_path, dest_path_str);
                        MediaDedup::ScannedFilesOps::updateFilePathInPreviewCache(sess, ejected_path, dest_path_str);
                        MediaDedup::EjectedFilesOps::deleteByEjectedPath(sess, ejected_path);
                        
                        sess.commit();
                    } catch (...) {
                        std::filesystem::remove(dest);
                        throw;
                    }
                    std::filesystem::remove(src);
                    
                    try {
                        std::filesystem::path parent = src.parent_path();
                        std::filesystem::path ds_store = parent / ".DS_Store";
                        if (std::filesystem::exists(ds_store)) std::filesystem::remove(ds_store);
                        if (std::filesystem::is_empty(parent)) {
                            std::filesystem::remove(parent);
                            MediaDedup::UserSettingsOps::remove(*db_, "ejectionPath:" + history.original_location_key);
                        }
                    } catch (...) {}
                    success_count++;
                } else {
                    std::filesystem::remove(dest);
                    failure_count++;
                }
            } catch (...) {
                failure_count++;
            }
        }
        
        reply->set_success(success_count > 0 || (success_count == 0 && failure_count == 0 && histories.empty()));
        reply->set_success_count(success_count);
        reply->set_failure_count(failure_count);
        return grpc::Status::OK;
    }

    grpc::Status SubscribeToDuplicateUpdates(grpc::ServerContext* context, const vurio::dedup::Empty* request,
                                             grpc::ServerWriter<vurio::dedup::DuplicateGroupUpdate>* writer) override {
        auto queue = std::make_shared<StreamQueue>();
        {
            std::lock_guard<std::mutex> lock(streams_mutex_);
            streams_.push_back(queue);
        }

        while (!context->IsCancelled()) {
            vurio::dedup::DuplicateGroupUpdate update;
            {
                std::unique_lock<std::mutex> lock(queue->mutex);
                queue->cv.wait_for(lock, std::chrono::seconds(1), [&queue, context]{
                    return !queue->updates.empty() || context->IsCancelled();
                });

                if (context->IsCancelled()) {
                    break;
                }

                if (queue->updates.empty()) {
                    continue; // Timeout, loop and check cancelled again
                }

                update = queue->updates.front();
                queue->updates.pop();
            }

            if (!writer->Write(update)) {
                break; // Connection closed
            }
        }

        {
            std::lock_guard<std::mutex> lock(streams_mutex_);
            streams_.erase(std::remove(streams_.begin(), streams_.end(), queue), streams_.end());
        }

        return grpc::Status::OK;
    }
};

static std::string calculateFileSHA1(const std::string& path) {
    Poco::SHA1Engine sha1;
    Poco::DigestOutputStream dos(sha1);
    Poco::FileInputStream fis(path);
    Poco::StreamCopier::copyStream(fis, dos);
    dos.close();
    return Poco::DigestEngine::digestToHex(sha1.digest());
}

class AdvancedSettingsServiceImpl final : public vurio::dedup::AdvancedSettingsService::Service {
    DatabaseManager* db_;
    std::shared_ptr<UnifiedObservableConfigManager> config_;
    std::shared_ptr<DiskCache> thumbnail_cache_;
    std::shared_ptr<DiskCache> transcoding_cache_;

public:
    explicit AdvancedSettingsServiceImpl(DatabaseManager* db, 
                                         std::shared_ptr<UnifiedObservableConfigManager> config, 
                                         std::shared_ptr<DiskCache> thumbnail_cache,
                                         std::shared_ptr<DiskCache> transcoding_cache)
        : db_(db), config_(std::move(config)),
          thumbnail_cache_(std::move(thumbnail_cache)), transcoding_cache_(std::move(transcoding_cache)) {}

    grpc::Status ResetErrors(grpc::ServerContext* context, const vurio::dedup::Empty* request,
                             vurio::dedup::AdvancedActionResponse* reply) override {
        try {
            int files_reset = MediaDedup::ScannedFilesOps::resetAllErrors(*db_);
            reply->set_success(files_reset >= 0);
            if (files_reset < 0) reply->set_error_message("Failed to reset errors");
        } catch (const std::exception& e) {
            reply->set_success(false);
            reply->set_error_message(e.what());
        }
        return grpc::Status::OK;
    }

    grpc::Status EjectErrors(grpc::ServerContext* context, const vurio::dedup::Empty* request,
                             vurio::dedup::AdvancedActionResponse* reply) override {
        try {
            auto errored_files = MediaDedup::ScannedFilesOps::listErroredFiles(*db_);
            int success_count = 0;
            int failure_count = 0;
            std::string last_error = "";

            for (const auto& file_opt : errored_files) {
                std::string file_path = file_opt.file_path;
                std::string location_key = file_opt.location_key;
                std::string ejection_path;

                if (!MediaDedup::UserSettingsOps::get(*db_, "ejectionPathErrored:" + location_key, ejection_path)) {
                    std::string media_location_path;
                    if (!MediaDedup::UserSettingsOps::get(*db_, location_key, media_location_path)) {
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
                        MediaDedup::UserSettingsOps::upsert(*db_, "ejectionPathErrored:" + location_key, ejection_path);
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
                        if (MediaDedup::UserSettingsOps::get(*db_, location_key, media_location_path)) {
                            new_relative_path = std::filesystem::relative(dest, std::filesystem::path(media_location_path)).string();
                            std::replace(new_relative_path.begin(), new_relative_path.end(), '\\', '/');
                        }

                        MediaDedup::EjectedFileHistory history;
                        history.file_id = file_opt.id;
                        history.ejected_path = new_file_path;
                        history.original_file_path = file_opt.file_path;
                        history.original_relative_path = file_opt.relative_path;
                        history.original_location_key = location_key;

                        try {
                            auto lease = db_->acquireSessionLease();
                            Poco::Data::Session& sess = lease.get();
                            sess.begin();

                            MediaDedup::EjectedFilesOps::insert(sess, history);
                            MediaDedup::ScannedFilesOps::updateFilePath(sess, file_opt.id, new_file_path, new_relative_path, location_key);
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
            
            reply->set_success(success_count > 0 || (success_count == 0 && failure_count == 0));
            reply->set_success_count(success_count);
            reply->set_failure_count(failure_count);
            if (success_count == 0 && !last_error.empty()) {
                reply->set_error_message(last_error);
            }
        } catch (const std::exception& e) {
            reply->set_success(false);
            reply->set_error_message(e.what());
        }
        return grpc::Status::OK;
    }

    grpc::Status UndoEjectErrors(grpc::ServerContext* context, const vurio::dedup::Empty* request,
                                 vurio::dedup::AdvancedActionResponse* reply) override {
        try {
            auto all_histories = MediaDedup::EjectedFilesOps::getAll(*db_);
            std::vector<MediaDedup::EjectedFileHistory> histories;
            for (const auto& hist : all_histories) {
                if (hist.ejected_path.find("_errored") != std::string::npos) {
                    histories.push_back(hist);
                }
            }

            int success_count = 0;
            int failure_count = 0;

            for (const auto& history : histories) {
                std::string ejected_path = history.ejected_path;
                std::string original_path = history.original_file_path;

                std::filesystem::path src(ejected_path);
                std::filesystem::path dest(original_path);

                if (!std::filesystem::exists(src)) {
                    MediaDedup::EjectedFilesOps::deleteByEjectedPath(*db_, ejected_path);
                    failure_count++;
                    continue;
                }

                try {
                    std::filesystem::copy_file(src, dest, std::filesystem::copy_options::overwrite_existing);

                    std::string src_sha1 = calculateFileSHA1(src.string());
                    std::string dest_sha1 = calculateFileSHA1(dest.string());

                    if (src_sha1 == dest_sha1) {
                        std::string dest_path_str = dest.string();
                        std::replace(dest_path_str.begin(), dest_path_str.end(), '\\', '/');

                        try {
                            auto lease = db_->acquireSessionLease();
                            Poco::Data::Session& sess = lease.get();
                            sess.begin();

                            MediaDedup::ScannedFilesOps::updateFilePath(sess, history.file_id, dest_path_str, history.original_relative_path, history.original_location_key);
                            MediaDedup::ScannedFilesOps::updateFilePathInImageArtifacts(sess, ejected_path, dest_path_str);
                            MediaDedup::ScannedFilesOps::updateFilePathInProcessingErrors(sess, ejected_path, dest_path_str);
                            MediaDedup::ScannedFilesOps::updateFilePathInDuplicateGroups(sess, ejected_path, dest_path_str);
                            MediaDedup::ScannedFilesOps::updateFilePathInDuplicateMembers(sess, ejected_path, dest_path_str);
                            MediaDedup::ScannedFilesOps::updateFilePathInThumbnailCache(sess, ejected_path, dest_path_str);

                            std::string dest_p = dest_path_str;
                            Poco::Data::Statement stmt_update_state(sess);
                            int unproc = 0;
                            stmt_update_state << "UPDATE scanned_files SET processed=? WHERE file_path=?", Poco::Data::Keywords::use(unproc), Poco::Data::Keywords::use(dest_p), Poco::Data::Keywords::now;

                            Poco::Data::Statement stmt_del_err(sess);
                            stmt_del_err << "DELETE FROM processing_errors WHERE file_path=?", Poco::Data::Keywords::use(dest_p), Poco::Data::Keywords::now;

                            MediaDedup::EjectedFilesOps::deleteByEjectedPath(sess, ejected_path);

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
                                MediaDedup::UserSettingsOps::remove(*db_, "ejectionPathErrored:" + history.original_location_key);
                            }
                        } catch (...) {}

                        success_count++;
                    } else {
                        std::filesystem::remove(dest);
                        failure_count++;
                    }
                } catch (const std::exception& e) {
                    failure_count++;
                }
            }

            reply->set_success(success_count > 0 || (success_count == 0 && failure_count == 0 && histories.empty()));
            reply->set_success_count(success_count);
            reply->set_failure_count(failure_count);
        } catch (const std::exception& e) {
            reply->set_success(false);
            reply->set_error_message(e.what());
        }
        return grpc::Status::OK;
    }

    grpc::Status EjectIgnored(grpc::ServerContext* context, const vurio::dedup::Empty* request,
                              vurio::dedup::AdvancedActionResponse* reply) override {
        try {
            auto ignored_files = MediaDedup::ScannedFilesOps::listIgnoredFiles(*db_);
            int success_count = 0;
            int failure_count = 0;
            std::string last_error = "";

            for (const auto& file_opt : ignored_files) {
                std::string file_path = file_opt.file_path;
                std::string location_key = file_opt.location_key;
                std::string ejection_path;

                if (!MediaDedup::UserSettingsOps::get(*db_, "ejectionPathIgnored:" + location_key, ejection_path)) {
                    std::string media_location_path;
                    if (!MediaDedup::UserSettingsOps::get(*db_, location_key, media_location_path)) {
                        last_error = "Cannot find registered media location for key: " + location_key;
                        failure_count++;
                        continue;
                    }
                    Poco::UUID uuid = Poco::UUIDGenerator::defaultGenerator().create();
                    std::filesystem::path root(media_location_path);
                    std::filesystem::path ejected = root / (uuid.toString() + "_ignored");
                    ejection_path = ejected.string();
                    std::replace(ejection_path.begin(), ejection_path.end(), '\\', '/');
                    
                    try {
                        std::filesystem::create_directories(ejected);
                        MediaDedup::UserSettingsOps::upsert(*db_, "ejectionPathIgnored:" + location_key, ejection_path);
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
                        if (MediaDedup::UserSettingsOps::get(*db_, location_key, media_location_path)) {
                            new_relative_path = std::filesystem::relative(dest, std::filesystem::path(media_location_path)).string();
                            std::replace(new_relative_path.begin(), new_relative_path.end(), '\\', '/');
                        }

                        MediaDedup::EjectedFileHistory history;
                        history.file_id = file_opt.id;
                        history.ejected_path = new_file_path;
                        history.original_file_path = file_opt.file_path;
                        history.original_relative_path = file_opt.relative_path;
                        history.original_location_key = location_key;

                        try {
                            auto lease = db_->acquireSessionLease();
                            Poco::Data::Session& sess = lease.get();
                            sess.begin();

                            MediaDedup::EjectedFilesOps::insert(sess, history);
                            MediaDedup::ScannedFilesOps::updateFilePath(sess, file_opt.id, new_file_path, new_relative_path, location_key);
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
            
            reply->set_success(success_count > 0 || (success_count == 0 && failure_count == 0));
            reply->set_success_count(success_count);
            reply->set_failure_count(failure_count);
            if (success_count == 0 && !last_error.empty()) {
                reply->set_error_message(last_error);
            }
        } catch (const std::exception& e) {
            reply->set_success(false);
            reply->set_error_message(e.what());
        }
        return grpc::Status::OK;
    }

    grpc::Status UndoEjectIgnored(grpc::ServerContext* context, const vurio::dedup::Empty* request,
                                  vurio::dedup::AdvancedActionResponse* reply) override {
        try {
            auto all_histories = MediaDedup::EjectedFilesOps::getAll(*db_);
            std::vector<MediaDedup::EjectedFileHistory> histories;
            for (const auto& hist : all_histories) {
                if (hist.ejected_path.find("_ignored") != std::string::npos) {
                    histories.push_back(hist);
                }
            }

            int success_count = 0;
            int failure_count = 0;

            for (const auto& history : histories) {
                std::string ejected_path = history.ejected_path;
                std::string original_path = history.original_file_path;

                std::filesystem::path src(ejected_path);
                std::filesystem::path dest(original_path);

                if (!std::filesystem::exists(src)) {
                    MediaDedup::EjectedFilesOps::deleteByEjectedPath(*db_, ejected_path);
                    failure_count++;
                    continue;
                }

                try {
                    std::filesystem::copy_file(src, dest, std::filesystem::copy_options::overwrite_existing);

                    std::string src_sha1 = calculateFileSHA1(src.string());
                    std::string dest_sha1 = calculateFileSHA1(dest.string());

                    if (src_sha1 == dest_sha1) {
                        std::string dest_path_str = dest.string();
                        std::replace(dest_path_str.begin(), dest_path_str.end(), '\\', '/');

                        try {
                            auto lease = db_->acquireSessionLease();
                            Poco::Data::Session& sess = lease.get();
                            sess.begin();

                            MediaDedup::ScannedFilesOps::updateFilePath(sess, history.file_id, dest_path_str, history.original_relative_path, history.original_location_key);
                            MediaDedup::ScannedFilesOps::updateFilePathInImageArtifacts(sess, ejected_path, dest_path_str);
                            MediaDedup::ScannedFilesOps::updateFilePathInProcessingErrors(sess, ejected_path, dest_path_str);
                            MediaDedup::ScannedFilesOps::updateFilePathInDuplicateGroups(sess, ejected_path, dest_path_str);
                            MediaDedup::ScannedFilesOps::updateFilePathInDuplicateMembers(sess, ejected_path, dest_path_str);
                            MediaDedup::ScannedFilesOps::updateFilePathInThumbnailCache(sess, ejected_path, dest_path_str);

                            std::string dest_p = dest_path_str;
                            Poco::Data::Statement stmt_update_state(sess);
                            int unproc = 0;
                            stmt_update_state << "UPDATE scanned_files SET processed=? WHERE file_path=?", Poco::Data::Keywords::use(unproc), Poco::Data::Keywords::use(dest_p), Poco::Data::Keywords::now;

                            MediaDedup::EjectedFilesOps::deleteByEjectedPath(sess, ejected_path);

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
                                MediaDedup::UserSettingsOps::remove(*db_, "ejectionPathIgnored:" + history.original_location_key);
                            }
                        } catch (...) {}

                        success_count++;
                    } else {
                        std::filesystem::remove(dest);
                        failure_count++;
                    }
                } catch (const std::exception& e) {
                    failure_count++;
                }
            }

            reply->set_success(success_count > 0 || (success_count == 0 && failure_count == 0 && histories.empty()));
            reply->set_success_count(success_count);
            reply->set_failure_count(failure_count);
        } catch (const std::exception& e) {
            reply->set_success(false);
            reply->set_error_message(e.what());
        }
        return grpc::Status::OK;
    }

    grpc::Status PurgeEjectedAndErrored(grpc::ServerContext* context, const vurio::dedup::Empty* request,
                                        vurio::dedup::AdvancedActionResponse* reply) override {
        try {
            auto all_histories = MediaDedup::EjectedFilesOps::getAll(*db_);
            int success_count = 0;
            int failure_count = 0;

            std::string purity_str = config_->getPropertyValue<std::string>("duplicates.purity.order", "CAMERA_RAW, EXIF_CAPTURE_DATE, FILE_SIZE");

            for (const auto& hist : all_histories) {
                if (hist.ejected_path.find("_ejected") != std::string::npos ||
                    hist.ejected_path.find("_errored") != std::string::npos) {
                    
                    auto groups = MediaDedup::DuplicateGroupsOps::getGroupsForFile(*db_, hist.file_id, "EMBEDDING");
                    for (const auto& g : groups) {
                        if (g.representative_file_id == hist.file_id) {
                            MediaDedup::Orchestration::DuplicateFinder::promoteNextBestRepresentative(*db_, g.id, hist.file_id, purity_str);
                        }
                    }

                    if (MediaDedup::ScannedFilesOps::completelyRemoveById(*db_, hist.file_id)) {
                        success_count++;
                    } else {
                        failure_count++;
                    }
                }
            }

            reply->set_success(success_count > 0 || (success_count == 0 && failure_count == 0));
            reply->set_success_count(success_count);
            reply->set_failure_count(failure_count);
        } catch (const std::exception& e) {
            reply->set_success(false);
            reply->set_error_message(e.what());
        }
        return grpc::Status::OK;
    }

    grpc::Status ResetDuplicates(grpc::ServerContext* context, const vurio::dedup::Empty* request,
                                 vurio::dedup::AdvancedActionResponse* reply) override {
        try {
            std::string mode = "EMBEDDING";
            bool success = true;
            if (!MediaDedup::DuplicateGroupsOps::deleteGroupsByMode(*db_, mode)) success = false;
            if (!MediaDedup::DuplicateGroupsOps::resetCheckpoint(*db_, mode)) success = false;
            reply->set_success(success);
            if (!success) reply->set_error_message("Failed to reset duplicates");
        } catch (const std::exception& e) {
            reply->set_success(false);
            reply->set_error_message(e.what());
        }
        return grpc::Status::OK;
    }

    grpc::Status StartDatabaseIntegrityCheck(grpc::ServerContext* context, const vurio::dedup::Empty* request,
                                             vurio::dedup::AdvancedActionResponse* reply) override {
        reply->set_success(true);
        return grpc::Status::OK;
    }

    grpc::Status GetDatabaseIntegrityCheckStatus(grpc::ServerContext* context, const vurio::dedup::Empty* request,
                                                 vurio::dedup::IntegrityCheckStatusResponse* reply) override {
        reply->set_running(false);
        return grpc::Status::OK;
    }

    grpc::Status BackupDatabase(grpc::ServerContext* context, const vurio::dedup::Empty* request,
                                vurio::dedup::BackupDatabaseResponse* reply) override {
        reply->set_success(true);
        return grpc::Status::OK;
    }

    grpc::Status GetLatestBackup(grpc::ServerContext* context, const vurio::dedup::Empty* request,
                                 vurio::dedup::BackupDatabaseResponse* reply) override {
        reply->set_success(true);
        return grpc::Status::OK;
    }

    grpc::Status ClearAllCaches(grpc::ServerContext* context, const vurio::dedup::Empty* request,
                                vurio::dedup::ClearCachesResponse* reply) override {
        reply->set_success(true);
        return grpc::Status::OK;
    }

    grpc::Status ConfigurePurityOrder(grpc::ServerContext* context, const vurio::dedup::ConfigurePurityOrderRequest* request,
                                      vurio::dedup::AdvancedActionResponse* reply) override {
        if (config_) {
            config_->setPropertyValue<std::string>("duplicates.purity.order", request->purity_order());
            config_->triggerSave();
            reply->set_success(true);
        } else {
            reply->set_success(false);
            reply->set_error_message("Config manager not available");
        }
        return grpc::Status::OK;
    }

    grpc::Status GetUserTheme(grpc::ServerContext* context, const vurio::dedup::Empty* request,
                              vurio::dedup::GetUserThemeResponse* reply) override {
        std::string theme = "os";
        if (db_) {
            MediaDedup::UserSettingsOps::get(*db_, "ui_theme", theme);
        }
        reply->set_theme(theme);
        return grpc::Status::OK;
    }

    grpc::Status SetUserTheme(grpc::ServerContext* context, const vurio::dedup::SetUserThemeRequest* request,
                              vurio::dedup::AdvancedActionResponse* reply) override {
        if (db_) {
            MediaDedup::UserSettingsOps::upsert(*db_, "ui_theme", request->theme());
            reply->set_success(true);
        } else {
            reply->set_success(false);
        }
        return grpc::Status::OK;
    }
};

GrpcServerCore::GrpcServerCore() {}

GrpcServerCore::~GrpcServerCore() {
    stop();
}

void GrpcServerCore::start(const std::string& socket_path,
                           std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                           DatabaseManager* database_manager,
                           std::shared_ptr<Orchestration::FilesManager> files_manager,
                           std::shared_ptr<ThreadPoolManager> thread_pool_manager,
                           std::shared_ptr<DiskCache> thumbnail_cache,
                           std::shared_ptr<DiskCache> transcoding_cache,
                           std::shared_ptr<Orchestration::DuplicateFinder> duplicate_finder) {
    socket_path_ = socket_path;
    
    // Ensure socket directory exists
    std::filesystem::path path(socket_path_);
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }

    if (std::filesystem::exists(socket_path_)) {
        std::filesystem::remove(socket_path_);
    }

    std::string server_address("unix:" + socket_path_);
    
    auto config_service = std::make_unique<ConfigServiceImpl>(config_manager);
    auto media_service = std::make_unique<MediaLocationsServiceImpl>(files_manager->getFilesService(), thread_pool_manager, database_manager);
    auto duplicates_service = std::make_unique<DuplicatesServiceImpl>(database_manager);
    std::unique_ptr<vurio::dedup::AdvancedSettingsService::Service> advanced_settings_service = std::make_unique<AdvancedSettingsServiceImpl>(database_manager, config_manager, thumbnail_cache, transcoding_cache);

    if (duplicate_finder) {
        // We use a raw pointer to the service because it's managed by services_ below,
        // and its lifetime is tied to the gRPC server which lives alongside DuplicateFinder
        DuplicatesServiceImpl* dup_svc = duplicates_service.get();
        duplicate_finder->setGroupEventCallback([dup_svc](int group_id, bool is_new) {
            dup_svc->OnDuplicateGroupEvent(group_id, is_new);
        });
    }

    grpc::ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(config_service.get());
    builder.RegisterService(media_service.get());
    builder.RegisterService(duplicates_service.get());
    builder.RegisterService(advanced_settings_service.get());
    
    services_.push_back(std::move(config_service));
    services_.push_back(std::move(media_service));
    services_.push_back(std::move(duplicates_service));
    services_.push_back(std::move(advanced_settings_service));

    server_ = builder.BuildAndStart();
    Poco::Logger::get("GrpcServerCore").information("gRPC Server listening on " + server_address);
}

void GrpcServerCore::stop() {
    if (server_) {
        server_->Shutdown();
        server_.reset();
        services_.clear();
        Poco::Logger::get("GrpcServerCore").information("gRPC Server stopped.");
    }
    if (!socket_path_.empty() && std::filesystem::exists(socket_path_)) {
        std::filesystem::remove(socket_path_);
    }
}

} // namespace Core
} // namespace MediaDedup
