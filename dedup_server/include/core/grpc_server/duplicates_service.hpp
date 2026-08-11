#pragma once

// AR-5: duplicate groups, eject, and update-subscription service.
// Moved verbatim out of the former grpc_server_core.cpp god file so the class
// is constructible from tests (which previously documented being unable to
// instantiate the TU-private impls).

#include "core/grpc_server/grpc_service_common.hpp"
#include "search/text_search_engine.hpp"

namespace MediaDedup {
namespace Core {

class DuplicatesServiceImpl final : public vurio::dedup::DuplicatesService::Service {
    std::shared_ptr<DatabaseManager> db_owner_; // CC-5
    DatabaseManager* db_;
    // Optional CLIP text->image search backend for the day-view search filter
    // (owned by GrpcServerCore; null in tests that don't exercise search).
    MediaDedup::TextSearchEngine* search_engine_ = nullptr;

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
    explicit DuplicatesServiceImpl(DatabaseManager* db,
                                   MediaDedup::TextSearchEngine* search_engine = nullptr)
        : db_(db), search_engine_(search_engine) {}

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

        broadcast(update);
    }

    /// The finder finished a re-cluster. It rebuilds every group for the mode
    /// in one transaction — ids are reused where a component still contains an
    /// old representative, groups appear and disappear, and a promoted
    /// representative changes the capture date a group sorts on — so there is
    /// no meaningful per-row delta to send. One RELOAD tells the client to
    /// re-fetch what it has on screen and re-anchor; the server stays the sole
    /// authority on ordering.
    void OnGroupsRebuilt() {
        vurio::dedup::DuplicateGroupUpdate update;
        update.set_type(vurio::dedup::DuplicateGroupUpdate::RELOAD);
        broadcast(update, /*coalesce=*/true);
    }

private:
    /// Fan one update out to every open subscriber. With `coalesce`, a queue
    /// whose tail is already the same type is left alone — a client that is
    /// slow to drain gets one reload, not a backlog of identical ones.
    void broadcast(const vurio::dedup::DuplicateGroupUpdate& update, bool coalesce = false) {
        std::lock_guard<std::mutex> lock(streams_mutex_);
        for (auto& queue : streams_) {
            std::lock_guard<std::mutex> q_lock(queue->mutex);
            if (coalesce && !queue->updates.empty() && queue->updates.back().type() == update.type())
                continue;
            queue->updates.push(update);
            queue->cv.notify_one();
        }
    }

public:

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

    // Resolve the request's semantic search filter (if any) into scanned_files
    // ids. Returns false (with `status` set) when the filter cannot be
    // evaluated; `match_ids` stays null when no filter is active.
    bool resolveSearchFilter(const std::string& query,
                             std::vector<int>& ids_storage,
                             const std::vector<int>*& match_ids,
                             grpc::Status& status) {
        match_ids = nullptr;
        if (query.empty()) return true;
        if (!search_engine_) {
            status = grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "Text search unavailable");
            return false;
        }
        auto search = search_engine_->search(query);
        if (!search.ok) {
            status = grpc::Status(grpc::StatusCode::UNAVAILABLE,
                                  "Text search failed: " + search.error);
            return false;
        }
        ids_storage.clear();
        ids_storage.reserve(search.matches.size());
        for (const auto& m : search.matches) ids_storage.push_back(m.file_id);
        match_ids = &ids_storage;
        return true;
    }

    grpc::Status GetDayGroups(grpc::ServerContext* context, const vurio::dedup::GetDayGroupsRequest* request,
                              vurio::dedup::GetDayGroupsResponse* reply) override {
        if (!db_) return grpc::Status(grpc::StatusCode::INTERNAL, "Database missing");

        int limit = request->limit() > 0 ? request->limit() : 30;
        int offset = request->offset() >= 0 ? request->offset() : 0;
        int files_per_day = request->files_per_day() > 0 ? request->files_per_day() : 0;

        std::vector<int> search_ids;
        const std::vector<int>* match_ids = nullptr;
        grpc::Status search_status = grpc::Status::OK;
        if (!resolveSearchFilter(request->search_query(), search_ids, match_ids, search_status))
            return search_status;

        auto page = MediaDedup::DayGroupsOps::getDayGroups(*db_, offset, limit, files_per_day, match_ids);
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

        std::vector<int> search_ids;
        const std::vector<int>* match_ids = nullptr;
        grpc::Status search_status = grpc::Status::OK;
        if (!resolveSearchFilter(request->search_query(), search_ids, match_ids, search_status))
            return search_status;

        auto page = MediaDedup::DayGroupsOps::getDayFiles(*db_, request->day(), offset, limit, match_ids);
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

    // True when `path` (slash-normalized) sits under any ejection root — the
    // same rule the list RPCs use to derive is_ejected.
    static bool isUnderEjectionPath(const std::string& path, const std::vector<std::string>& ejection_paths) {
        std::string normalized = path;
        std::replace(normalized.begin(), normalized.end(), '\\', '/');
        for (const auto& ep : ejection_paths) {
            if (normalized.find(ep) == 0) return true;
        }
        return false;
    }

    grpc::Status GetEligibleDuplicatePaths(grpc::ServerContext* context,
                                           const vurio::dedup::GetEligibleDuplicatePathsRequest* request,
                                           vurio::dedup::DuplicatePathsResponse* reply) override {
        if (!db_) return grpc::Status(grpc::StatusCode::INTERNAL, "Database missing");

        // Score/representative filtering is done in SQL; the ejected exclusion
        // happens here because ejection roots are dynamic user settings.
        auto ejection_paths = loadEjectionPaths();
        auto paths = MediaDedup::DuplicateGroupsOps::getEjectCandidatePaths(*db_, request->tightness());
        for (auto& path : paths) {
            if (!isUnderEjectionPath(path, ejection_paths)) {
                reply->add_file_paths(std::move(path));
            }
        }
        return grpc::Status::OK;
    }

    grpc::Status GetEjectedDuplicatePaths(grpc::ServerContext* context, const vurio::dedup::Empty* request,
                                          vurio::dedup::DuplicatePathsResponse* reply) override {
        if (!db_) return grpc::Status(grpc::StatusCode::INTERNAL, "Database missing");

        // Deliberately scoped to duplicate-group members (not the full ejection
        // history): the BINNED Restore button restores duplicates only —
        // errored/ignored ejects are restored from Settings.
        auto ejection_paths = loadEjectionPaths();
        auto paths = MediaDedup::DuplicateGroupsOps::getAllMemberPaths(*db_);
        for (auto& path : paths) {
            if (isUnderEjectionPath(path, ejection_paths)) {
                reply->add_file_paths(std::move(path));
            }
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

    grpc::Status GetGroupIndex(grpc::ServerContext* context, const vurio::dedup::GetGroupIndexRequest* request,
                               vurio::dedup::GetGroupIndexResponse* reply) override {
        if (!db_) return grpc::Status(grpc::StatusCode::INTERNAL, "Database missing");

        int group_id = 0;
        try {
            group_id = std::stoi(request->group_id());
        } catch (const std::exception&) {
            reply->set_found(false);
            return grpc::Status::OK;
        }

        int rank = MediaDedup::DuplicateGroupsOps::getGroupRank(*db_, group_id);
        reply->set_found(rank >= 0);
        reply->set_index(rank >= 0 ? rank : 0);
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

} // namespace Core
} // namespace MediaDedup
