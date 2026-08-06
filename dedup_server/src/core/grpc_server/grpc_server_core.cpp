#include "core/grpc_server/grpc_server_core.hpp"
#include <grpcpp/grpcpp.h>
#include <Poco/Logger.h>
#include <filesystem>
#include "utils/full_res_lru_cache.hpp"
#include "dedup.grpc.pb.h"
#include "config/unified_observable_config.hpp"
#include "database/database_manager.hpp"
#include "orchestration/files_manager.hpp"
#include <nlohmann/json.hpp>
#include "database/scanned_files_ops.hpp"
#include "database/duplicate_groups_ops.hpp"
#include "database/day_groups_ops.hpp"
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
#include "filesmanager/media_availability.hpp"
#include "orchestration/integrity_check_service.hpp"
#include "services/eject_service.hpp"
#include "media_processors/media_processor.hpp"
#include "media_processors/thumbnail_service.hpp"
#include "database/thumbnail_cache_ops.hpp"
#include "database/preview_cache_ops.hpp"
#include "orchestration/scheduler_service.hpp"
#include "utils/thumbnail_generator.hpp"
#include "media_processors/image/backends/transcoding_pipeline.hpp"
#include "core/media_ready_bus.hpp"
#include "core/media_generation_failures.hpp"
#include "database/processing_errors_ops.hpp"
#include "utils/temp_paths.hpp"
#include <unordered_map>
#include <chrono>
#include <sstream>
#include <mutex>
#include <random>
#include <fstream>
#include <cstdlib>
namespace MediaDedup {
namespace Core {

namespace {
    // Per-server-instance pairing secrets, set in GrpcServerCore::start() before
    // the server accepts connections. The cookie is written to a 0600 file that
    // only the same OS user can read; the session token is handed back by
    // Handshake and must accompany every other call. Both are random per process
    // start, so a client paired with one server instance cannot talk to another.
    std::mutex g_auth_mutex;
    std::string g_pairing_cookie;
    std::string g_session_token;

    std::string generateSecretHex(std::size_t bytes = 32) {
        std::random_device rd;
        static const char* kHex = "0123456789abcdef";
        std::string out;
        out.reserve(bytes * 2);
        for (std::size_t i = 0; i < bytes; ++i) {
            unsigned int b = rd() & 0xFFu;
            out.push_back(kHex[(b >> 4) & 0xF]);
            out.push_back(kHex[b & 0xF]);
        }
        return out;
    }

    bool constantTimeEquals(const std::string& a, const std::string& b) {
        if (a.empty() || a.size() != b.size()) return false;
        unsigned char diff = 0;
        for (std::size_t i = 0; i < a.size(); ++i)
            diff |= static_cast<unsigned char>(a[i] ^ b[i]);
        return diff == 0;
    }

    bool constantTimeEqualsRef(const grpc::string_ref& a, const std::string& b) {
        if (b.empty() || a.size() != b.size()) return false;
        const char* ad = a.data();
        unsigned char diff = 0;
        for (std::size_t i = 0; i < b.size(); ++i)
            diff |= static_cast<unsigned char>(ad[i] ^ b[i]);
        return diff == 0;
    }

    // AR-4: session enforcement for EVERY method in ONE place. The processor
    // runs before any handler is invoked and closes unauthenticated calls
    // with UNAUTHENTICATED — replacing the identical guard that was
    // copy-pasted into 34 of 35 RPCs (and would have had to be remembered in
    // every future one). Handshake is exempt: it is how a paired client
    // obtains the token in the first place. The pairing design itself
    // (cookie file readable only by the same OS user, unix socket,
    // constant-time compares) is unchanged.
    class SessionAuthProcessor : public grpc::AuthMetadataProcessor {
    public:
        grpc::Status Process(const InputMetadata& auth_metadata, grpc::AuthContext* /*context*/,
                             OutputMetadata* /*consumed_auth_metadata*/,
                             OutputMetadata* /*response_metadata*/) override {
            auto path = auth_metadata.find(":path");
            if (path != auth_metadata.end() &&
                path->second == grpc::string_ref("/vurio.dedup.SecurityService/Handshake")) {
                return grpc::Status::OK;
            }
            std::string expected;
            { std::lock_guard<std::mutex> lk(g_auth_mutex); expected = g_session_token; }
            if (expected.empty()) {
                return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Server not paired");
            }
            auto it = auth_metadata.find("x-vurio-token");
            if (it == auth_metadata.end() || !constantTimeEqualsRef(it->second, expected)) {
                return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Invalid or missing session token");
            }
            return grpc::Status::OK;
        }
    };
}

class SecurityServiceImpl final : public vurio::dedup::SecurityService::Service {
public:
    grpc::Status Handshake(grpc::ServerContext* /*context*/,
                           const vurio::dedup::HandshakeRequest* request,
                           vurio::dedup::HandshakeResponse* reply) override {
        std::string cookie, token;
        { std::lock_guard<std::mutex> lk(g_auth_mutex); cookie = g_pairing_cookie; token = g_session_token; }
        if (!cookie.empty() && constantTimeEquals(request->cookie(), cookie)) {
            reply->set_ok(true);
            reply->set_session_token(token);
            Poco::Logger::get("GrpcSecurity").information("Client handshake succeeded; session established");
        } else {
            reply->set_ok(false);
            reply->set_error_message("Handshake failed: invalid pairing cookie");
            Poco::Logger::get("GrpcSecurity").warning("Client handshake rejected (invalid pairing cookie)");
        }
        return grpc::Status::OK;
    }
};

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
    std::shared_ptr<DatabaseManager> db_owner_; // CC-5
    DatabaseManager* db_;
    std::shared_ptr<MediaDedup::Orchestration::SchedulerService> scheduler_;
public:
    explicit MediaLocationsServiceImpl(std::shared_ptr<FilesService> f, std::shared_ptr<ThreadPoolManager> tpm, DatabaseManager* db, std::shared_ptr<MediaDedup::Orchestration::SchedulerService> scheduler) 
        : files_(std::move(f)), tpm_(std::move(tpm)), db_(db), scheduler_(std::move(scheduler)) {}

    grpc::Status GetStatus(grpc::ServerContext* context, const vurio::dedup::GetStatusRequest* request,
                           vurio::dedup::GetStatusResponse* reply) override {
        auto locations = files_->listMediaLocations();
        for (const auto& pair : locations) {
            reply->add_registered_dirs(pair.second);
        }
        
        if (db_) {
            auto checkpoint = MediaDedup::DuplicateGroupsOps::getCheckpoint(*db_, "EMBEDDING");
            int last_processed_id = checkpoint.has_value() ? checkpoint->last_processed_id : 0;

            // DB-7: one aggregated query + one lease for all five
            // scanned_files counts (this poll runs every 2 s).
            MediaDedup::ScannedFilesOps::ScanStatusCounts counts;
            MediaDedup::ScannedFilesOps::getStatusCounts(*db_, last_processed_id, counts);
            reply->set_scanned_files_count(counts.total);
            reply->set_processed_files_count(counts.processed);
            reply->set_error_files_count(counts.errors);
            reply->set_duplicate_finding_total(counts.processed_success);
            reply->set_duplicate_finding_completed(counts.processed_up_to);

            auto stats = MediaDedup::DuplicateGroupsOps::getStats(*db_, "EMBEDDING");
            reply->set_duplicate_groups_count(stats.total_groups);
            reply->set_duplicate_files_count(stats.total_duplicates);
            
            auto user_settings = MediaDedup::UserSettingsOps::list(*db_);
            for (const auto& kv : user_settings) {
                if (kv.first.rfind("ejectionPath:", 0) == 0 ||
                    kv.first.rfind("ejectionPathErrored:", 0) == 0 ||
                    kv.first.rfind("ejectionPathIgnored:", 0) == 0) {
                    reply->add_ejected_dirs(kv.second);
                }
            }
        }
        if (tpm_) {
            auto tpm_status = tpm_->getStatus();
            reply->set_thread_count(tpm_status.runningTotal);
            reply->set_max_threads(tpm_status.effectiveMax);
        }
        if (scheduler_) {
            reply->set_is_paused(scheduler_->isPaused());
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
        if (tpm_) {
            Poco::Logger::get("DedupServiceImpl").information("Clearing thread pool queues prior to deregistering a path.");
            tpm_->clearQueues();
        }
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

    grpc::Status LocateMissingFile(grpc::ServerContext* context, const vurio::dedup::LocateMissingFileRequest* request,
                                   vurio::dedup::LocateMissingFileResponse* reply) override {
        auto result = files_->locateMissingFile(request->member_path(), request->located_path());
        reply->set_success(result.success);
        reply->set_error_message(result.error_message);
        reply->set_files_updated(result.files_updated);
        reply->set_old_path(result.old_path);
        reply->set_new_path(result.new_path);
        return grpc::Status::OK;
    }
};

class DuplicatesServiceImpl final : public vurio::dedup::DuplicatesService::Service {
    std::shared_ptr<DatabaseManager> db_owner_; // CC-5
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
            proto_file->set_id(std::to_string(member.file_id));
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

        // Snapshot the mount table once so we can flag members on disconnected volumes.
        auto mounted_fs = MediaDedup::MediaAvailability::mountedFilesystems();

        for (const auto& group_with_members : page.groups) {
            if (group_with_members.group.member_count < 2) continue;
            
            auto* proto_group = reply->add_groups();
            proto_group->set_group_id(std::to_string(group_with_members.group.id));
            
            for (const auto& member : group_with_members.members) {
                auto* proto_file = proto_group->add_files();
                proto_file->set_id(std::to_string(member.file_id));
                proto_file->set_file_path(member.file_path);
                proto_file->set_size_bytes(member.file_size);
                proto_file->set_is_representative(member.is_representative);
                proto_file->set_similarity_score(member.similarity_score);
                if (!member.cached_thumbnail_path.empty()) {
                    proto_file->set_cached_thumbnail_path(member.cached_thumbnail_path);
                }
                
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

                proto_file->set_is_available(
                    MediaDedup::MediaAvailability::isPathAvailable(member.file_path, mounted_fs));
            }
        }
        return grpc::Status::OK;
    }

    // Ejection roots ("ejectionPath:" user settings, slashes normalized) used to
    // flag day files already moved into an ejection bin. DB-7: cached with a
    // short TTL — this ran a full user_settings scan (plus a pool lease) on
    // EVERY list RPC; ejection paths change only via the settings UI, and the
    // frontend reloads its views after changing them anyway.
    std::vector<std::string> loadEjectionPaths() {
        static std::mutex cache_mutex;
        static std::vector<std::string> cached;
        static std::chrono::steady_clock::time_point cached_at{};
        constexpr auto kTtl = std::chrono::seconds(5);

        const auto now_tp = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(cache_mutex);
            if (cached_at != std::chrono::steady_clock::time_point{} && now_tp - cached_at < kTtl)
                return cached;
        }

        auto user_settings = MediaDedup::UserSettingsOps::list(*db_);
        std::vector<std::string> ejection_paths;
        for (const auto& kv : user_settings) {
            if (kv.first.rfind("ejectionPath:", 0) == 0) {
                std::string ep = kv.second;
                std::replace(ep.begin(), ep.end(), '\\', '/');
                ejection_paths.push_back(ep);
            }
        }

        {
            std::lock_guard<std::mutex> lock(cache_mutex);
            cached = ejection_paths;
            cached_at = now_tp;
        }
        return ejection_paths;
    }

    // Map one DayFileRecord onto its proto, including the derived
    // is_ejected/is_available flags. Shared by GetDayGroups and GetDayFiles.
    void fillDayFileProto(vurio::dedup::DayFile* proto_file,
                          const MediaDedup::DayFileRecord& file,
                          const std::vector<std::string>& ejection_paths,
                          const std::vector<std::string>& mounted_fs) {
        proto_file->set_id(std::to_string(file.file_id));
        proto_file->set_file_path(file.file_path);
        proto_file->set_taken_time(file.taken_time);
        proto_file->set_size_bytes(file.size_bytes);
        if (!file.cached_thumbnail_path.empty()) {
            proto_file->set_cached_thumbnail_path(file.cached_thumbnail_path);
        }
        if (file.group_id > 0) {
            proto_file->set_group_id(std::to_string(file.group_id));
            proto_file->set_is_representative(file.is_representative);
            proto_file->set_similarity_score(file.similarity_score);
        }

        bool is_ejected = false;
        std::string normalized_path = file.file_path;
        std::replace(normalized_path.begin(), normalized_path.end(), '\\', '/');
        for (const auto& ep : ejection_paths) {
            if (normalized_path.find(ep) == 0) {
                is_ejected = true;
                break;
            }
        }
        proto_file->set_is_ejected(is_ejected);

        proto_file->set_is_available(
            MediaDedup::MediaAvailability::isPathAvailable(file.file_path, mounted_fs));
    }

    grpc::Status GetDayGroups(grpc::ServerContext* context, const vurio::dedup::GetDayGroupsRequest* request,
                              vurio::dedup::GetDayGroupsResponse* reply) override {
        if (!db_) return grpc::Status(grpc::StatusCode::INTERNAL, "Database missing");

        int limit = request->limit() > 0 ? request->limit() : 30;
        int offset = request->offset() >= 0 ? request->offset() : 0;
        int files_per_day = request->files_per_day() > 0 ? request->files_per_day() : 0;

        auto page = MediaDedup::DayGroupsOps::getDayGroups(*db_, offset, limit, files_per_day);
        if (!page.ok) {
            // Transient failure (e.g. DB session pool exhausted under load).
            // Returning OK with total_days=0 would make the client treat the
            // library as empty and wipe its day list — surface an error so the
            // client keeps its state and retries.
            return grpc::Status(grpc::StatusCode::UNAVAILABLE, "Day view temporarily busy");
        }
        reply->set_total_days(page.total_days);

        auto ejection_paths = loadEjectionPaths();

        // Snapshot the mount table once so we can flag files on disconnected volumes.
        auto mounted_fs = MediaDedup::MediaAvailability::mountedFilesystems();

        for (const auto& day : page.days) {
            auto* proto_day = reply->add_days();
            proto_day->set_day(day.day);
            proto_day->set_total_files(day.total_files);

            for (const auto& file : day.files) {
                fillDayFileProto(proto_day->add_files(), file, ejection_paths, mounted_fs);
            }
        }
        return grpc::Status::OK;
    }

    grpc::Status GetDayFiles(grpc::ServerContext* context, const vurio::dedup::GetDayFilesRequest* request,
                             vurio::dedup::GetDayFilesResponse* reply) override {
        if (!db_) return grpc::Status(grpc::StatusCode::INTERNAL, "Database missing");
        if (request->day().empty()) {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "day is required");
        }

        int limit = request->limit() > 0 ? request->limit() : 60;
        int offset = request->offset() >= 0 ? request->offset() : 0;

        auto page = MediaDedup::DayGroupsOps::getDayFiles(*db_, request->day(), offset, limit);
        if (!page.ok) {
            return grpc::Status(grpc::StatusCode::UNAVAILABLE, "Day view temporarily busy");
        }
        reply->set_total_files(page.total_files);

        auto ejection_paths = loadEjectionPaths();
        auto mounted_fs = MediaDedup::MediaAvailability::mountedFilesystems();

        for (const auto& file : page.files) {
            fillDayFileProto(reply->add_files(), file, ejection_paths, mounted_fs);
        }
        return grpc::Status::OK;
    }

    grpc::Status EjectDuplicates(grpc::ServerContext* context, const vurio::dedup::EjectDuplicatesRequest* request,
                                 vurio::dedup::EjectDuplicatesResponse* reply) override {
        // Delegate to the shared, optimized bulk eject (rename fast path,
        // single batched transaction) used by the HTTP handler too.
        std::vector<std::string> paths(request->file_paths().begin(), request->file_paths().end());
        auto r = MediaDedup::Services::bulkEject(*db_, paths);
        reply->set_success(r.successCount > 0 || (r.failureCount == 0 && r.skippedUnavailable > 0));
        reply->set_success_count(r.successCount);
        reply->set_failure_count(r.failureCount);
        reply->set_skipped_unavailable(r.skippedUnavailable);
        reply->set_error_message(r.lastError);
        return grpc::Status::OK;
    }

    grpc::Status UndoEjectDuplicates(grpc::ServerContext* context, const vurio::dedup::UndoEjectDuplicatesRequest* request,
                                     vurio::dedup::UndoEjectDuplicatesResponse* reply) override {
        std::vector<std::string> paths(request->file_paths().begin(), request->file_paths().end());
        auto r = MediaDedup::Services::bulkUndoEject(*db_, paths, request->undo_all());
        reply->set_success(r.successCount > 0 || (r.failureCount == 0));
        reply->set_success_count(r.successCount);
        reply->set_failure_count(r.failureCount);
        reply->set_skipped_unavailable(r.skippedUnavailable);
        reply->set_error_message(r.lastError);
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

// Session-lifetime index of generated full-resolution previews (temp files):
// path -> {temp jpeg, source mtime}. Mirrors the web endpoint's
// no-persistent-cache semantics while making repeat zooms instant within a
// server run. File-scope so both MediaServiceImpl (fills it) and
// AdvancedSettingsServiceImpl::ClearAllCaches (resets it, QW-9) can reach it.
//
// MP-4: bounded LRU (entry + byte caps) — the previous unbounded map never
// deleted its quality-95 native-resolution temp files during the process
// lifetime (macOS only clears /var/folders on reboot): scrolling the viewer
// through 500 photos leaked ~5 GB invisible to every cache limit. Eviction,
// staleness replacement, clear, and shutdown all unlink the temp render.
static MediaDedup::FullResLruCache& fullResCache() {
    static MediaDedup::FullResLruCache c; return c;
}
static void clearFullResCache() { fullResCache().clear(); }

class AdvancedSettingsServiceImpl final : public vurio::dedup::AdvancedSettingsService::Service {
    std::shared_ptr<DatabaseManager> db_owner_; // CC-5
    DatabaseManager* db_;
    std::shared_ptr<UnifiedObservableConfigManager> config_;
    std::shared_ptr<DiskCache> thumbnail_cache_;
    std::shared_ptr<DiskCache> transcoding_cache_;
    std::shared_ptr<MediaDedup::Orchestration::SchedulerService> scheduler_;
    std::shared_ptr<MediaDedup::ThreadPoolManager> tpm_;

public:
    // CC-5: owns a real share of the DB; db_ stays as the raw convenience
    // pointer the method bodies use.
    explicit AdvancedSettingsServiceImpl(std::shared_ptr<DatabaseManager> db,
                                         std::shared_ptr<UnifiedObservableConfigManager> config, 
                                         std::shared_ptr<DiskCache> thumbnail_cache,
                                         std::shared_ptr<DiskCache> transcoding_cache,
                                         std::shared_ptr<MediaDedup::Orchestration::SchedulerService> scheduler,
                                         std::shared_ptr<MediaDedup::ThreadPoolManager> tpm)
        : db_owner_(std::move(db)), db_(db_owner_.get()), config_(std::move(config)),
          thumbnail_cache_(std::move(thumbnail_cache)), transcoding_cache_(std::move(transcoding_cache)),
          scheduler_(std::move(scheduler)), tpm_(std::move(tpm)) {}

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

    // AR-2: category ejects + purge delegate to the shared eject service —
    // the previous inline clones lacked filename-collision handling, ignored
    // DB-write failures (still deleting sources), and skipped row/history
    // cleanup. The service carries the correct (former HTTP) logic.

    static void fillBulkReply(vurio::dedup::AdvancedActionResponse* reply,
                              const MediaDedup::Services::BulkOpResult& r) {
        reply->set_success(r.successCount > 0 || r.failureCount == 0);
        reply->set_success_count(r.successCount);
        reply->set_failure_count(r.failureCount);
        if (r.successCount == 0 && !r.lastError.empty()) {
            reply->set_error_message(r.lastError);
        }
    }

    grpc::Status EjectErrors(grpc::ServerContext* context, const vurio::dedup::Empty* request,
                             vurio::dedup::AdvancedActionResponse* reply) override {
        try {
            fillBulkReply(reply, MediaDedup::Services::ejectErrored(*db_));
        } catch (const std::exception& e) {
            reply->set_success(false);
            reply->set_error_message(e.what());
        }
        return grpc::Status::OK;
    }

    grpc::Status UndoEjectErrors(grpc::ServerContext* context, const vurio::dedup::Empty* request,
                                 vurio::dedup::AdvancedActionResponse* reply) override {
        try {
            fillBulkReply(reply, MediaDedup::Services::undoEjectErrored(*db_));
        } catch (const std::exception& e) {
            reply->set_success(false);
            reply->set_error_message(e.what());
        }
        return grpc::Status::OK;
    }

    grpc::Status EjectIgnored(grpc::ServerContext* context, const vurio::dedup::Empty* request,
                              vurio::dedup::AdvancedActionResponse* reply) override {
        try {
            fillBulkReply(reply, MediaDedup::Services::ejectIgnored(*db_));
        } catch (const std::exception& e) {
            reply->set_success(false);
            reply->set_error_message(e.what());
        }
        return grpc::Status::OK;
    }

    grpc::Status UndoEjectIgnored(grpc::ServerContext* context, const vurio::dedup::Empty* request,
                                  vurio::dedup::AdvancedActionResponse* reply) override {
        try {
            fillBulkReply(reply, MediaDedup::Services::undoEjectIgnored(*db_));
        } catch (const std::exception& e) {
            reply->set_success(false);
            reply->set_error_message(e.what());
        }
        return grpc::Status::OK;
    }

    grpc::Status PurgeEjectedAndErrored(grpc::ServerContext* context, const vurio::dedup::Empty* request,
                                        vurio::dedup::AdvancedActionResponse* reply) override {
        try {
            fillBulkReply(reply, MediaDedup::Services::purgeEjected(*db_, config_));
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

    grpc::Status PauseProcessing(grpc::ServerContext* context, const vurio::dedup::Empty* request,
                                 vurio::dedup::AdvancedActionResponse* reply) override {
        if (scheduler_) {
            scheduler_->pause();
            reply->set_success(true);
        } else {
            reply->set_success(false);
            reply->set_error_message("Scheduler not available");
        }
        return grpc::Status::OK;
    }

    grpc::Status ResumeProcessing(grpc::ServerContext* context, const vurio::dedup::Empty* request,
                                 vurio::dedup::AdvancedActionResponse* reply) override {
        if (scheduler_) {
            scheduler_->resume();
            reply->set_success(true);
        } else {
            reply->set_success(false);
            reply->set_error_message("Scheduler not available");
        }
        return grpc::Status::OK;
    }

    grpc::Status StartDatabaseIntegrityCheck(grpc::ServerContext* context, const vurio::dedup::Empty* request,
                                             vurio::dedup::AdvancedActionResponse* reply) override {
        std::string error;
        if (MediaDedup::Orchestration::IntegrityCheckService::start(db_owner_, config_, scheduler_, tpm_, error)) {
            reply->set_success(true);
        } else {
            reply->set_success(false);
            reply->set_error_message(error);
        }
        return grpc::Status::OK;
    }

    grpc::Status GetDatabaseIntegrityCheckStatus(grpc::ServerContext* context, const vurio::dedup::Empty* request,
                                                 vurio::dedup::IntegrityCheckStatusResponse* reply) override {
        auto state = MediaDedup::Orchestration::IntegrityCheckService::status();
        reply->set_running(state.is_running);
        reply->set_progress(state.progress);
        reply->set_total_files(state.total_files);
        reply->set_processed_files(state.processed_files);
        reply->set_files_removed(state.files_removed);
        reply->set_groups_removed(state.groups_removed);
        reply->set_thumbnails_removed(state.thumbnails_removed);
        reply->set_artifacts_removed(state.artifacts_removed);
        reply->set_errors_removed(state.errors_removed);
        reply->set_current_phase(state.current_phase);
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
        try {
            bool thumb_success = thumbnail_cache_->clearCache();
            bool transcode_success = transcoding_cache_->clearCache();

            try {
                MediaDedup::ThumbnailCacheOps::clearCache(*db_);
                MediaDedup::PreviewCacheOps::clearCache(*db_);
            } catch (...) {
                // Ignore DB clear errors, similar to REST endpoint
            }

            // QW-9: a user-initiated clear is a full reset, not just a disk
            // wipe. Negative-cached files (3 failed generations) must get a
            // fresh attempt — same as a server restart — including their
            // persisted MediaGeneration error rows; and the viewer's
            // session-lifetime full-res renders must go too, or it keeps
            // serving pre-clear pixels.
            MediaDedup::MediaGenerationFailures::instance().clearAll();
            try {
                MediaDedup::ProcessingErrorsOps::deleteBySource(*db_, "MediaGeneration");
            } catch (...) {
                // Same tolerance as the cache-table clears above.
            }
            clearFullResCache();

            reply->set_success(thumb_success && transcode_success);
            reply->set_thumbnail_cache_cleared(thumb_success);
            reply->set_disk_cache_cleared(transcode_success);
        } catch (const std::exception& e) {
            reply->set_success(false);
            reply->set_error_message(e.what());
        }
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

class MediaServiceImpl final : public vurio::dedup::MediaService::Service {
    std::shared_ptr<UnifiedObservableConfigManager> config_;
    std::shared_ptr<DatabaseManager> db_owner_; // CC-5
    DatabaseManager* db_;
    std::shared_ptr<DiskCache> thumbnail_cache_;
    std::shared_ptr<DiskCache> transcoding_cache_;
    std::shared_ptr<ThreadPoolManager> tpm_;

public:
    // CC-5: owns a real share of the DB so the fire-and-forget generation
    // lambdas below can capture genuine ownership instead of laundering a
    // raw pointer through a no-op-deleter shared_ptr.
    MediaServiceImpl(std::shared_ptr<UnifiedObservableConfigManager> config,
                     std::shared_ptr<DatabaseManager> db,
                     std::shared_ptr<DiskCache> thumb_cache, 
                     std::shared_ptr<DiskCache> trans_cache, 
                     std::shared_ptr<ThreadPoolManager> tpm)
        : config_(config), db_owner_(std::move(db)), db_(db_owner_.get()), thumbnail_cache_(thumb_cache), transcoding_cache_(trans_cache), tpm_(tpm) {}

    // mtime of the source file, or 0 when unreadable (e.g. its volume is
    // disconnected — cached assets are still served in that case).
    static int64_t sourceModifiedTime(const std::string& path) {
        try {
            auto ftime = std::filesystem::last_write_time(path);
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
            return std::chrono::system_clock::to_time_t(sctp);
        } catch (...) {
            return 0;
        }
    }

    // Threshold-crossing bookkeeping shared by the thumbnail and preview
    // generation lambdas: count the failure, persist one processing_errors row
    // the moment the file becomes negative-cached, and push the failure event.
    static void recordGenerationFailure(DatabaseManager* db, const std::string& path,
                                        const std::string& media_type, const std::string& message) {
        int count = MediaDedup::MediaGenerationFailures::instance().recordFailure(path, media_type, message);
        if (count == MediaDedup::MediaGenerationFailures::FAILURE_THRESHOLD && db) {
            MediaDedup::ProcessingErrorsOps::insertError(*db, path, MediaDedup::ServerMode::EMBEDDING, -6,
                                                         message, "MediaGeneration");
        }
        MediaDedup::MediaReadyBus::instance().publish({path, media_type, false, message});
    }

    grpc::Status GetThumbnail(grpc::ServerContext* context, const vurio::dedup::GetMediaRequest* request,
                              vurio::dedup::MediaPathResponse* response) override {
        std::string file_path = request->file_path();
        int size = 80;
        try { size = std::stoi(request->size()); } catch (...) {}

        int normalized_size = ThumbnailGenerator::getClosestValidSize(size);

        // Negative cache: a file that failed generation repeatedly gets a
        // distinct, terminal answer instead of an endless UNAVAILABLE loop.
        if (auto reason = MediaDedup::MediaGenerationFailures::instance().permanentFailureReason(file_path, "thumbnail")) {
            return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, *reason);
        }

        bool lookup_failed = false;
        auto cached = MediaDedup::ThumbnailCacheOps::getBestFitThumbnail(*db_, file_path, normalized_size, &lookup_failed);
        if (lookup_failed) {
            // DB session pool starved — the asset may well be cached. Let the
            // client retry the lookup; do NOT submit a fresh generation.
            return grpc::Status(grpc::StatusCode::UNAVAILABLE, "Thumbnail cache lookup busy");
        }

        // Serve the cache when it is fresh. mtime 0 (source unreadable, e.g.
        // disconnected volume) still serves the cached asset.
        int64_t source_mtime = sourceModifiedTime(file_path);
        if (cached && std::filesystem::exists(cached->cached_path) &&
            (source_mtime == 0 || cached->source_modified_at == 0 || cached->source_modified_at == source_mtime)) {
            response->set_file_path(cached->cached_path);
            return grpc::Status::OK;
        } else {
            if (tpm_) {
                auto config_copy = config_;
                auto db_copy = db_owner_; // CC-5: owning share
                auto tcache_copy = thumbnail_cache_;
                auto xcache_copy = transcoding_cache_;
                auto tpm_copy = tpm_;
                std::string path_copy = file_path;
                int size_copy = normalized_size;

                tpm_->submit("thumbnail_generator", [config_copy, db_copy, tcache_copy, path_copy, size_copy]() {
                    MediaDedup::ThumbnailService service(config_copy, db_copy, tcache_copy);
                    std::string cached_path;
                    MediaDedup::ThumbnailService::GenerationFailure kind;
                    if (service.generateAndCacheThumbnail(path_copy, size_copy, cached_path, &kind)) {
                        MediaDedup::MediaGenerationFailures::instance().recordSuccess(path_copy, "thumbnail");
                        MediaDedup::MediaReadyBus::instance().publish({path_copy, "thumbnail", true, ""});
                    } else if (kind == MediaDedup::ThumbnailService::GenerationFailure::Decode) {
                        recordGenerationFailure(db_copy.get(), path_copy, "thumbnail",
                                                "Thumbnail generation failed (unsupported or corrupted file)");
                    } else {
                        // Infrastructure trouble (offline volume, cache write
                        // error): wake waiters so the client can retry later,
                        // but never count it toward the negative cache.
                        MediaDedup::MediaReadyBus::instance().publish(
                            {path_copy, "thumbnail", false, "Thumbnail generation failed (transient server error)"});
                    }
                }, file_path, /*urgent=*/true);
            }
            return grpc::Status(grpc::StatusCode::UNAVAILABLE, "Thumbnail generating");
        }
    }

    grpc::Status GetPreview(grpc::ServerContext* context, const vurio::dedup::GetMediaRequest* request,
                            vurio::dedup::MediaPathResponse* response) override {
        std::string file_path = request->file_path();

        // Full-resolution request (viewer zoomed in). The native client asks
        // this only for RAW files (it decodes standard formats locally); the
        // handler is format-agnostic anyway. Same generation call as the web
        // endpoint's size=full (native res, quality 95), but async + pushed
        // via MediaReady like every other gRPC media generation.
        if (request->size() == "full") {
            if (auto reason = MediaDedup::MediaGenerationFailures::instance().permanentFailureReason(file_path, "preview_full")) {
                return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, *reason);
            }

            int64_t full_mtime = sourceModifiedTime(file_path);
            if (auto hit = fullResCache().getFresh(file_path, full_mtime)) {
                response->set_file_path(hit->temp_path);
                response->set_is_full_resolution(true);
                return grpc::Status::OK;
            }

            if (tpm_) {
                auto db_copy = db_owner_; // CC-5: owning share
                auto config_copy = config_;
                std::string path_copy = file_path;
                tpm_->submit("fullres_generator", [db_copy, config_copy, path_copy]() {
                    int timeout_ms = config_copy
                        ? config_copy->getPropertyValue<int>("media.image.transcoding.timeoutMs", ThumbnailGenerator::DEFAULT_TIMEOUT_MS)
                        : ThumbnailGenerator::DEFAULT_TIMEOUT_MS;
                    Magick::Blob blob;
                    // size 0 = native resolution, quality 95 — matches the web endpoint
                    if (ThumbnailGenerator::generateToBlob(path_copy, ThumbnailGenerator::kNativeResolution, blob, 95, timeout_ms)) {
                        std::string out_path = MediaDedup::makeTempFilePath("fullres", ".jpg");
                        std::ofstream out(out_path, std::ios::binary);
                        out.write(static_cast<const char*>(blob.data()), static_cast<std::streamsize>(blob.length()));
                        out.close();
                        if (!out.fail()) {
                            fullResCache().put(path_copy, {out_path, sourceModifiedTime(path_copy)});
                            MediaDedup::MediaGenerationFailures::instance().recordSuccess(path_copy, "preview_full");
                            MediaDedup::MediaReadyBus::instance().publish({path_copy, "preview_full", true, ""});
                            return;
                        }
                    }
                    recordGenerationFailure(db_copy.get(), path_copy, "preview_full",
                                            "Full-resolution generation failed (unsupported or corrupted file)");
                }, file_path, /*urgent=*/true);
            }
            return grpc::Status(grpc::StatusCode::UNAVAILABLE, "Full resolution generating");
        }

        // Best-available: when a full-resolution render is already materialized
        // for this file, serve it for the ordinary preview request too — no
        // point handing out QHD only for the viewer to upgrade on zoom.
        if (auto hit = fullResCache().getFresh(file_path, sourceModifiedTime(file_path))) {
            response->set_file_path(hit->temp_path);
            response->set_is_full_resolution(true);
            return grpc::Status::OK;
        }

        if (!TranscodingPipeline::NeedsQhdPreviewCache(file_path, config_)) {
            // File does not need transcoding (e.g., standard JPEG/PNG) — the
            // original path IS the full-resolution image.
            response->set_file_path(file_path);
            response->set_is_full_resolution(true);
            return grpc::Status::OK;
        }

        if (auto reason = MediaDedup::MediaGenerationFailures::instance().permanentFailureReason(file_path, "preview")) {
            return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, *reason);
        }

        bool lookup_failed = false;
        auto cached = MediaDedup::PreviewCacheOps::getPreview(*db_, file_path, &lookup_failed);
        if (lookup_failed) {
            return grpc::Status(grpc::StatusCode::UNAVAILABLE, "Preview cache lookup busy");
        }

        int64_t source_mtime = sourceModifiedTime(file_path);
        if (cached && std::filesystem::exists(cached->cached_path) &&
            (source_mtime == 0 || cached->source_modified_at == 0 || cached->source_modified_at == source_mtime)) {
            response->set_file_path(cached->cached_path);
            return grpc::Status::OK;
        } else {
            if (tpm_) {
                auto config_copy = config_;
                auto db_copy = db_owner_; // CC-5: owning share
                auto xcache_copy = transcoding_cache_;
                std::string path_copy = file_path;
                
                tpm_->submit("preview_generator", [config_copy, db_copy, xcache_copy, path_copy]() {
                    TranscodingConfig transcode_config = TranscodingPipeline::GetConfigFromManager(config_copy);
                    if (transcode_config.enabled) {
                        std::string temp_dir = std::filesystem::temp_directory_path().string();
                        std::string temp_path;
                        if (TranscodingPipeline::TranscodeToFile(path_copy, temp_path, transcode_config, temp_dir)) {
                            std::string final_path;
                            if (xcache_copy && xcache_copy->moveToCache(temp_path, final_path)) {
                                PreviewCacheOps::upsertPreview(
                                    *db_copy, PreviewCacheOps::makeRecord(path_copy, final_path));

                                // AR-1: the dead WebSocket broadcast is gone —
                                // MediaReadyBus serves the live (gRPC) client.
                                MediaDedup::MediaGenerationFailures::instance().recordSuccess(path_copy, "preview");
                                MediaDedup::MediaReadyBus::instance().publish({path_copy, "preview", true, ""});
                            } else {
                                std::filesystem::remove(temp_path);
                                recordGenerationFailure(db_copy.get(), path_copy, "preview",
                                                        "Failed to cache transcoded preview");
                            }
                        } else {
                            recordGenerationFailure(db_copy.get(), path_copy, "preview",
                                                    "Preview generation failed (unsupported or corrupted file)");
                        }
                    }
                }, file_path, /*urgent=*/true);
            }
            return grpc::Status(grpc::StatusCode::UNAVAILABLE, "Preview generating");
        }
    }

    grpc::Status CancelMediaGeneration(grpc::ServerContext* context, const vurio::dedup::CancelMediaRequest* request,
                                       vurio::dedup::Empty* response) override {
        if (tpm_) {
            tpm_->cancelTask(request->type(), request->file_path());
        }
        return grpc::Status::OK;
    }

    grpc::Status SubscribeToMediaReady(grpc::ServerContext* context, const vurio::dedup::Empty* request,
                                       grpc::ServerWriter<vurio::dedup::MediaReadyEvent>* writer) override {
        auto subscription = MediaDedup::MediaReadyBus::instance().subscribe();

        while (!context->IsCancelled()) {
            MediaDedup::MediaReadyBus::Event event;
            {
                std::unique_lock<std::mutex> lock(subscription->mutex);
                subscription->cv.wait_for(lock, std::chrono::seconds(1), [&subscription, context]{
                    return !subscription->events.empty() || context->IsCancelled();
                });

                if (context->IsCancelled()) {
                    break;
                }
                if (subscription->events.empty()) {
                    continue; // Timeout, loop and check cancelled again
                }

                event = subscription->events.front();
                subscription->events.pop();
            }

            vurio::dedup::MediaReadyEvent proto_event;
            proto_event.set_file_path(event.file_path);
            proto_event.set_media_type(event.media_type);
            proto_event.set_success(event.success);
            proto_event.set_error_message(event.error_message);

            if (!writer->Write(proto_event)) {
                break; // Connection closed
            }
        }

        MediaDedup::MediaReadyBus::instance().unsubscribe(subscription);
        return grpc::Status::OK;
    }
};

GrpcServerCore::GrpcServerCore() {}

GrpcServerCore::~GrpcServerCore() {
    stop();
}

void GrpcServerCore::start(const std::string& socket_path,
                           std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                           std::shared_ptr<DatabaseManager> database_manager,
                           std::shared_ptr<Orchestration::FilesManager> files_manager,
                           std::shared_ptr<ThreadPoolManager> thread_pool_manager,
                           std::shared_ptr<DiskCache> thumbnail_cache,
                           std::shared_ptr<DiskCache> transcoding_cache,
                           std::shared_ptr<Orchestration::DuplicateFinder> duplicate_finder,
                           std::shared_ptr<Orchestration::SchedulerService> scheduler_service) {
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

    // Generate this instance's pairing secrets and publish the cookie to a 0600
    // file only the same OS user can read. Done before BuildAndStart so the
    // token is in place before any call is accepted.
    {
        std::string cookie = generateSecretHex();
        std::string token = generateSecretHex();
        { std::lock_guard<std::mutex> lk(g_auth_mutex); g_pairing_cookie = cookie; g_session_token = token; }
        const char* home = std::getenv("HOME");
        if (home && *home) {
            std::error_code ec;
            std::filesystem::path dir = std::filesystem::path(home) / ".vurio";
            std::filesystem::create_directories(dir, ec);
            std::filesystem::permissions(dir, std::filesystem::perms::owner_all,
                                         std::filesystem::perm_options::replace, ec);
            std::filesystem::path cookie_file = dir / "grpc.cookie";
            { std::ofstream ofs(cookie_file, std::ios::trunc); ofs << cookie; }
            std::filesystem::permissions(cookie_file,
                std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                std::filesystem::perm_options::replace, ec);
            Poco::Logger::get("GrpcServerCore").information("Wrote gRPC pairing cookie to " + cookie_file.string());
        } else {
            Poco::Logger::get("GrpcServerCore").error("HOME not set; gRPC clients will not be able to pair");
        }
    }

    database_manager_ = database_manager; // CC-5 keep-alive for raw db_ members
    auto security_service = std::make_unique<SecurityServiceImpl>();
    auto config_service = std::make_unique<ConfigServiceImpl>(config_manager);
    std::shared_ptr<MediaDedup::FilesService> files_service = nullptr;
    if (files_manager) {
        files_service = files_manager->getFilesService();
    }
    auto media_service = std::make_unique<MediaLocationsServiceImpl>(files_service, thread_pool_manager, database_manager.get(), scheduler_service);
    auto duplicates_service = std::make_unique<DuplicatesServiceImpl>(database_manager.get());
    std::unique_ptr<vurio::dedup::AdvancedSettingsService::Service> advanced_settings_service = std::make_unique<AdvancedSettingsServiceImpl>(database_manager, config_manager, thumbnail_cache, transcoding_cache, scheduler_service, thread_pool_manager);
    auto media_streaming_service = std::make_unique<MediaServiceImpl>(config_manager, database_manager, thumbnail_cache, transcoding_cache, thread_pool_manager);

    duplicate_finder_ = duplicate_finder; // CC-6: for callback teardown in stop()
    if (duplicate_finder) {
        // We use a raw pointer to the service because it's managed by services_ below;
        // stop() clears this callback before services_ teardown (CC-6).
        DuplicatesServiceImpl* dup_svc = duplicates_service.get();
        duplicate_finder->setGroupEventCallback([dup_svc](int group_id, bool is_new) {
            dup_svc->OnDuplicateGroupEvent(group_id, is_new);
        });
    }

    grpc::ServerBuilder builder;
    // AR-4: the auth processor is attached to the server credentials so the
    // session check runs before ANY handler, for every present and future RPC.
    // Local (UDS) credentials, not insecure ones: identical wire format (no
    // TLS), but they carry a real security context, which is what makes
    // SetAuthMetadataProcessor work — on InsecureServerCredentials gRPC
    // aborts with "Not Supported" (verified empirically on gRPC 1.80).
    auto server_creds = grpc::experimental::LocalServerCredentials(UDS);
    server_creds->SetAuthMetadataProcessor(std::make_shared<SessionAuthProcessor>());
    builder.AddListeningPort(server_address, server_creds);
    // AR-4: bound the sync server's worker threads — previously unbounded,
    // so a thumbnail-scroll burst could spawn threads without limit.
    grpc::ResourceQuota quota("vurio-grpc");
    quota.SetMaxThreads(32);
    builder.SetResourceQuota(quota);
    builder.RegisterService(security_service.get());
    builder.RegisterService(config_service.get());
    builder.RegisterService(media_service.get());
    builder.RegisterService(duplicates_service.get());
    builder.RegisterService(advanced_settings_service.get());
    builder.RegisterService(media_streaming_service.get());
    
    services_.push_back(std::move(security_service));
    services_.push_back(std::move(config_service));
    services_.push_back(std::move(media_service));
    services_.push_back(std::move(duplicates_service));
    services_.push_back(std::move(advanced_settings_service));
    services_.push_back(std::move(media_streaming_service));

    server_ = builder.BuildAndStart();

    // Restrict the socket to the owner so only the same OS user can connect.
    {
        std::error_code ec;
        std::filesystem::permissions(socket_path_,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace, ec);
    }

    Poco::Logger::get("GrpcServerCore").information("gRPC Server listening on " + server_address);
}

void GrpcServerCore::stop() {
    // CC-6: detach the finder's group-event callback BEFORE tearing down the
    // service it points at — a still-running dedup task could otherwise call
    // into freed memory during the shutdown drain window.
    if (duplicate_finder_) {
        duplicate_finder_->setGroupEventCallback(nullptr);
        duplicate_finder_.reset();
    }
    if (server_) {
        // CC-6: deadline — an un-deadlined Shutdown blocks until every RPC
        // returns; the 1 s-polling streams alone stall it, and a stuck unary
        // handler hangs shutdown forever. In-flight RPCs get 5 s, then are
        // cancelled.
        server_->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(5));
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
