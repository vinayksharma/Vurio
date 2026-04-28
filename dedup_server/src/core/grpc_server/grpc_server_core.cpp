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
                json_config[key] = property->getValueAsString();
            }
        }
        reply->set_json_payload(json_config.dump(4));
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
            reply->set_duplicate_groups_count(MediaDedup::DuplicateGroupsOps::countGroupsByMode(*db_, "EMBEDDING"));
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
};

GrpcServerCore::GrpcServerCore() {}

GrpcServerCore::~GrpcServerCore() {
    stop();
}

void GrpcServerCore::start(const std::string& socket_path,
                           std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                           DatabaseManager* database_manager,
                           std::shared_ptr<Orchestration::FilesManager> files_manager,
                           std::shared_ptr<ThreadPoolManager> thread_pool_manager) {
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

    grpc::ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(config_service.get());
    builder.RegisterService(media_service.get());
    builder.RegisterService(duplicates_service.get());
    
    services_.push_back(std::move(config_service));
    services_.push_back(std::move(media_service));
    services_.push_back(std::move(duplicates_service));

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
