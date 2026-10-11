#pragma once

// AR-5: duplicate groups, eject, and update-subscription service.
// Moved verbatim out of the former grpc_server_core.cpp god file so the class
// is constructible from tests (which previously documented being unable to
// instantiate the TU-private impls).

#include "database/faces_ops.hpp"
#include "config/unified_observable_config.hpp"
#include "core/grpc_server/grpc_service_common.hpp"
#include "adjustments/adjustment_store.hpp"
#include "search/text_search_engine.hpp"

#include <unordered_map>
#include <utility>

namespace MediaDedup {
namespace Core {

class DuplicatesServiceImpl final : public vurio::dedup::DuplicatesService::Service {
    std::shared_ptr<DatabaseManager> db_owner_; // CC-5
    DatabaseManager* db_;
    // Optional CLIP text->image search backend for the day-view search filter
    // (owned by GrpcServerCore; null in tests that don't exercise search).
    MediaDedup::TextSearchEngine* search_engine_ = nullptr;
    // FACE-8: live config for the merge-purge / suspect-fit cutoff (null in
    // tests -> the kSuspectFitBar default applies).
    std::shared_ptr<UnifiedObservableConfigManager> config_;

    // The unified FACE-8 knob: one value drives the popup's amber suspect
    // ring AND the merge-time purge, so what's flagged is what a merge
    // would set aside. 0 disables purging (the ring then never shows).
    double faceFitCutoff() const {
        return config_ ? config_->getPropertyValue<double>(
                             "media.face.mergePurgeCutoff",
                             MediaDedup::FacesOps::kSuspectFitBar)
                       : MediaDedup::FacesOps::kSuspectFitBar;
    }

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
                                   MediaDedup::TextSearchEngine* search_engine = nullptr,
                                   std::shared_ptr<UnifiedObservableConfigManager> config = nullptr)
        : db_(db), search_engine_(search_engine), config_(std::move(config)) {}

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
            if (!member.cached_thumbnail_path.empty()) {
                proto_file->set_cached_thumbnail_path(member.cached_thumbnail_path);
            }
            for (const auto& ref : member.cached_thumbnails) { // STRIP-2
                auto* ct = proto_file->add_cached_thumbnails();
                ct->set_path(ref.path);
                ct->set_size(ref.size);
            }
            // THUMB-1: display geometry; 0 is the meaningful "unknown".
            // ADJ-1: the base dims from the row, with the file's edit chain
            // folded in, plus the revision that keys its cached assets.
            {
                MediaDedup::DisplayDims dims = MediaDedup::Adjustments::AdjustmentStore::instance().displayDimsFor(
                    member.file_path, member.display_width, member.display_height);
                proto_file->set_display_width(dims.width);
                proto_file->set_display_height(dims.height);
                proto_file->set_render_revision(MediaDedup::Adjustments::AdjustmentStore::instance().revisionFor(member.file_path));
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
                for (const auto& ref : member.cached_thumbnails) { // STRIP-2
                    auto* ct = proto_file->add_cached_thumbnails();
                    ct->set_path(ref.path);
                    ct->set_size(ref.size);
                }
                // THUMB-1: display geometry; 0 is the meaningful "unknown".
                // ADJ-1: see populateFile — chain folded in, revision carried.
                {
                    MediaDedup::DisplayDims dims = MediaDedup::Adjustments::AdjustmentStore::instance().displayDimsFor(
                        member.file_path, member.display_width, member.display_height);
                    proto_file->set_display_width(dims.width);
                    proto_file->set_display_height(dims.height);
                    proto_file->set_render_revision(MediaDedup::Adjustments::AdjustmentStore::instance().revisionFor(member.file_path));
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
    // file_id -> per-route match attribution for the active search's union.
    struct MatchFlags {
        bool ai = false;
        bool exif = false;
        bool face = false;
    };
    using SearchMatchFlags = std::unordered_map<int, MatchFlags>;

    void fillDayFileProto(vurio::dedup::DayFile* proto_file,
                          const MediaDedup::DayFileRecord& file,
                          const std::vector<std::string>& ejection_paths,
                          const std::vector<std::string>& mounted_fs,
                          const SearchMatchFlags* match_flags = nullptr) {
        if (match_flags) {
            auto it = match_flags->find(file.file_id);
            if (it != match_flags->end()) {
                proto_file->set_matched_ai(it->second.ai);
                proto_file->set_matched_exif(it->second.exif);
                proto_file->set_matched_face(it->second.face);
            }
        }
        proto_file->set_id(std::to_string(file.file_id));
        proto_file->set_file_path(file.file_path);
        proto_file->set_taken_time(file.taken_time);
        proto_file->set_size_bytes(file.size_bytes);
        if (!file.place_label.empty())
            proto_file->set_place_label(file.place_label);
        if (!file.cached_thumbnail_path.empty()) {
            proto_file->set_cached_thumbnail_path(file.cached_thumbnail_path);
        }
        for (const auto& ref : file.cached_thumbnails) { // STRIP-2
            auto* ct = proto_file->add_cached_thumbnails();
            ct->set_path(ref.path);
            ct->set_size(ref.size);
        }
        if (file.group_id > 0) {
            proto_file->set_group_id(std::to_string(file.group_id));
            proto_file->set_is_representative(file.is_representative);
            proto_file->set_similarity_score(file.similarity_score);
        }
        // THUMB-1: display geometry; 0 is the meaningful "unknown".
        // ADJ-1: the row carries the auto-oriented base; the file's edit
        // chain shapes what the client lays out, and the revision keys its
        // thumbnail cache entries.
        {
            MediaDedup::DisplayDims dims = MediaDedup::Adjustments::AdjustmentStore::instance().displayDimsFor(
                file.file_path, file.display_width, file.display_height);
            proto_file->set_display_width(dims.width);
            proto_file->set_display_height(dims.height);
            proto_file->set_render_revision(MediaDedup::Adjustments::AdjustmentStore::instance().revisionFor(file.file_path));
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

    // FACE-3: restrict the day view to one cluster's photos. Composes with
    // an active search by intersection; alone, the cluster's file ids ARE
    // the match list (order irrelevant — the day view groups by day).
    void applyClusterFilter(int cluster_id,
                            std::vector<int>& ids_storage,
                            const std::vector<int>*& match_ids) {
        if (cluster_id <= 0 || !db_) return;
        auto cluster_ids = MediaDedup::FacesOps::listClusterFileIds(*db_, cluster_id);
        if (match_ids == nullptr) {
            ids_storage = std::move(cluster_ids);
        } else {
            std::unordered_set<int> keep(cluster_ids.begin(), cluster_ids.end());
            std::vector<int> both;
            both.reserve(ids_storage.size());
            for (int id : ids_storage)
                if (keep.count(id))
                    both.push_back(id);
            ids_storage = std::move(both);
        }
        match_ids = &ids_storage;
    }

public:
    grpc::Status ListFaceClusters(grpc::ServerContext*, const vurio::dedup::ListFaceClustersRequest* request,
                                  vurio::dedup::ListFaceClustersResponse* reply) override {
        if (!db_) return grpc::Status(grpc::StatusCode::INTERNAL, "Database missing");
        for (const auto& c : MediaDedup::FacesOps::listClusterInfos(*db_, request->include_hidden())) {
            auto* p = reply->add_clusters();
            p->set_id(c.id);
            p->set_name(c.name);
            p->set_face_count(c.face_count);
            p->set_file_count(c.file_count);
            p->set_cover_file_path(c.cover_file_path);
            p->set_cover_x(c.cover_x);
            p->set_cover_y(c.cover_y);
            p->set_cover_w(c.cover_w);
            p->set_cover_h(c.cover_h);
            p->set_hidden(c.hidden);
            for (const auto& a : c.aliases)
                p->add_aliases(a);
            p->set_cover_pinned(c.cover_pinned);
            p->set_cover_face_id(c.cover_face_id);
            p->set_person_id(c.person_id);
        }
        return grpc::Status::OK;
    }

    grpc::Status NameFaceCluster(grpc::ServerContext*, const vurio::dedup::NameFaceClusterRequest* request,
                                 vurio::dedup::FaceClusterActionResponse* reply) override {
        if (!db_) return grpc::Status(grpc::StatusCode::INTERNAL, "Database missing");
        bool ok = MediaDedup::FacesOps::nameCluster(*db_, request->cluster_id(), request->name());
        // FACE-5: aliases ride the same call (replace-all semantics). Only
        // meaningful while the cluster is named.
        if (ok && !request->name().empty()) {
            const int pid = MediaDedup::FacesOps::personIdByName(*db_, request->name());
            if (pid > 0) {
                std::vector<std::string> aliases(request->aliases().begin(),
                                                 request->aliases().end());
                ok = MediaDedup::FacesOps::setPersonAliases(*db_, pid, aliases);
                // FACE-7a: naming a cluster with an existing name merges the
                // person's clusters into one — the same-name act is the
                // user's identity statement. FACE-8: the merged result is
                // purged of misfit faces at the configured cutoff.
                if (ok) {
                    int purged = 0;
                    ok = MediaDedup::FacesOps::mergePersonClusters(
                             *db_, pid, faceFitCutoff(), &purged) > 0;
                    reply->set_purged_faces(purged);
                }
            }
        }
        reply->set_success(ok);
        if (!ok) reply->set_error_message("Failed to name cluster");
        return grpc::Status::OK;
    }

    grpc::Status MergeFaceClusters(grpc::ServerContext*, const vurio::dedup::MergeFaceClustersRequest* request,
                                   vurio::dedup::FaceClusterActionResponse* reply) override {
        if (!db_) return grpc::Status(grpc::StatusCode::INTERNAL, "Database missing");
        std::vector<int> sources(request->source_ids().begin(), request->source_ids().end());
        int purged = 0;
        const bool ok = MediaDedup::FacesOps::mergeClusters(
            *db_, request->target_id(), sources, faceFitCutoff(), &purged);
        reply->set_success(ok);
        reply->set_purged_faces(purged);
        if (!ok) reply->set_error_message("Failed to merge clusters");
        return grpc::Status::OK;
    }

    grpc::Status SetFaceClusterHidden(grpc::ServerContext*, const vurio::dedup::SetFaceClusterHiddenRequest* request,
                                      vurio::dedup::FaceClusterActionResponse* reply) override {
        if (!db_) return grpc::Status(grpc::StatusCode::INTERNAL, "Database missing");
        const bool ok = MediaDedup::FacesOps::setClusterHidden(*db_, request->cluster_id(), request->hidden());
        reply->set_success(ok);
        if (!ok) reply->set_error_message("Failed to update cluster");
        return grpc::Status::OK;
    }

    grpc::Status ListClusterFaces(grpc::ServerContext*, const vurio::dedup::ListClusterFacesRequest* request,
                                  vurio::dedup::ListClusterFacesResponse* reply) override {
        if (!db_) return grpc::Status(grpc::StatusCode::INTERNAL, "Database missing");
        int total = 0;
        const int limit = request->limit() > 0 ? request->limit() : 200;
        for (const auto& f : MediaDedup::FacesOps::listClusterFaces(
                 *db_, request->cluster_id(), request->offset(), limit, total,
                 faceFitCutoff())) {
            auto* p = reply->add_faces();
            p->set_face_id(f.face_id);
            p->set_file_path(f.file_path);
            p->set_x(f.x);
            p->set_y(f.y);
            p->set_w(f.w);
            p->set_h(f.h);
            p->set_quality(f.quality);
            p->set_fit(f.fit);
            p->set_suspect(f.suspect);
        }
        reply->set_total(total);
        return grpc::Status::OK;
    }

    grpc::Status RemoveFaceFromCluster(grpc::ServerContext*, const vurio::dedup::RemoveFaceFromClusterRequest* request,
                                       vurio::dedup::FaceClusterActionResponse* reply) override {
        if (!db_) return grpc::Status(grpc::StatusCode::INTERNAL, "Database missing");
        const bool ok = MediaDedup::FacesOps::removeFaceFromCluster(*db_, request->face_id());
        reply->set_success(ok);
        if (!ok) reply->set_error_message("Failed to remove face");
        return grpc::Status::OK;
    }

    grpc::Status DissolveFaceCluster(grpc::ServerContext*, const vurio::dedup::DissolveFaceClusterRequest* request,
                                     vurio::dedup::FaceClusterActionResponse* reply) override {
        if (!db_) return grpc::Status(grpc::StatusCode::INTERNAL, "Database missing");
        const bool ok = MediaDedup::FacesOps::dissolveCluster(*db_, request->cluster_id());
        reply->set_success(ok);
        if (!ok) reply->set_error_message("Failed to dissolve cluster");
        return grpc::Status::OK;
    }

    grpc::Status SetFaceClusterCover(grpc::ServerContext*, const vurio::dedup::SetFaceClusterCoverRequest* request,
                                     vurio::dedup::FaceClusterActionResponse* reply) override {
        if (!db_) return grpc::Status(grpc::StatusCode::INTERNAL, "Database missing");
        const bool ok = MediaDedup::FacesOps::setClusterCover(
            *db_, request->cluster_id(), request->face_id());
        reply->set_success(ok);
        // The popup may race the sweep re-homing the face elsewhere.
        if (!ok) reply->set_error_message("face is not in this cluster");
        return grpc::Status::OK;
    }

    // The ai/xf/fc toggles select the ROUTES the query runs through, not a
    // post-filter: switched-off routes are stripped from the parse and the
    // query re-runs over what remains. (A bound hybrid result is an
    // intersection whose every match carries all its flags, so a
    // post-filter was vacuous there.) All selected — or all false, from an
    // older client — is the default everything-on hybrid.
    static MediaDedup::TextSearchEngine::Routes toggleRoutes(bool include_ai, bool include_exif,
                                                             bool include_face) {
        if (!include_ai && !include_exif && !include_face)
            return {};
        return {include_ai, include_exif, include_face};
    }

    // Resolve the request's semantic search filter (if any) into scanned_files
    // ids. Returns false (with `status` set) when the filter cannot be
    // evaluated; `match_ids` stays null when no filter is active.
    bool resolveSearchFilter(const std::string& query,
                             std::vector<int>& ids_storage,
                             const std::vector<int>*& match_ids,
                             grpc::Status& status,
                             bool* used_ai = nullptr,
                             bool* used_exif = nullptr,
                             SearchMatchFlags* match_flags = nullptr,
                             bool* parse_degraded = nullptr,
                             bool include_ai = true,
                             bool include_exif = true,
                             bool* used_face = nullptr,
                             bool include_face = true) {
        match_ids = nullptr;
        if (query.empty()) return true;
        if (!search_engine_) {
            status = grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "Text search unavailable");
            return false;
        }
        auto search = search_engine_->search(
            query, toggleRoutes(include_ai, include_exif, include_face));
        if (!search.ok) {
            status = grpc::Status(grpc::StatusCode::UNAVAILABLE,
                                  "Text search failed: " + search.error);
            return false;
        }
        if (used_ai) *used_ai = search.used_semantic;
        if (used_exif) *used_exif = search.used_exif;
        if (used_face) *used_face = search.used_face;
        if (parse_degraded) *parse_degraded = search.parse_degraded;
        ids_storage.clear();
        ids_storage.reserve(search.matches.size());
        for (const auto& m : search.matches) {
            ids_storage.push_back(m.file_id);
            if (match_flags) (*match_flags)[m.file_id] = {m.via_semantic, m.via_exif, m.via_face};
        }
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
        bool used_ai = false, used_exif = false, used_face = false, parse_degraded = false;
        SearchMatchFlags match_flags;
        if (!resolveSearchFilter(request->search_query(), search_ids, match_ids, search_status,
                                 &used_ai, &used_exif, &match_flags, &parse_degraded,
                                 request->search_include_ai(), request->search_include_exif(),
                                 &used_face, request->search_include_face()))
            return search_status;
        applyClusterFilter(request->filter_cluster_id(), search_ids, match_ids);
        reply->set_search_used_ai(used_ai);
        reply->set_search_used_exif(used_exif);
        reply->set_search_used_face(used_face);
        reply->set_search_parse_degraded(parse_degraded);

        // Per-route sizes of the result set the day list is about to show.
        // Counted over match_ids (not match_flags) so an intersected cluster
        // filter is reflected; routes overlap, so these do not sum to the total.
        if (match_ids) {
            int count_ai = 0, count_exif = 0, count_face = 0;
            for (int file_id : *match_ids) {
                auto it = match_flags.find(file_id);
                if (it == match_flags.end()) continue;
                if (it->second.ai) ++count_ai;
                if (it->second.exif) ++count_exif;
                if (it->second.face) ++count_face;
            }
            reply->set_search_match_count_ai(count_ai);
            reply->set_search_match_count_exif(count_exif);
            reply->set_search_match_count_face(count_face);
        }

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
                fillDayFileProto(proto_day->add_files(), file, ejection_paths, mounted_fs,
                                 match_ids ? &match_flags : nullptr);
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
        bool used_ai = false, used_exif = false, used_face = false, parse_degraded = false;
        SearchMatchFlags match_flags;
        if (!resolveSearchFilter(request->search_query(), search_ids, match_ids, search_status,
                                 &used_ai, &used_exif, &match_flags, &parse_degraded,
                                 request->search_include_ai(), request->search_include_exif(),
                                 &used_face, request->search_include_face()))
            return search_status;
        applyClusterFilter(request->filter_cluster_id(), search_ids, match_ids);
        reply->set_search_used_ai(used_ai);
        reply->set_search_used_exif(used_exif);
        reply->set_search_used_face(used_face);
        reply->set_search_parse_degraded(parse_degraded);

        auto page = MediaDedup::DayGroupsOps::getDayFiles(*db_, request->day(), offset, limit, match_ids);
        if (!page.ok) {
            return grpc::Status(grpc::StatusCode::UNAVAILABLE, "Day view temporarily busy");
        }
        reply->set_total_files(page.total_files);

        auto ejection_paths = loadEjectionPaths();
        auto mounted_fs = MediaDedup::MediaAvailability::mountedFilesystems();

        for (const auto& file : page.files) {
            fillDayFileProto(reply->add_files(), file, ejection_paths, mounted_fs,
                             match_ids ? &match_flags : nullptr);
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
