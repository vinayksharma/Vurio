#pragma once

// AR-5: advanced settings / maintenance actions service.
// Moved verbatim out of the former grpc_server_core.cpp god file so the class
// is constructible from tests (which previously documented being unable to
// instantiate the TU-private impls).

#include "core/grpc_server/grpc_service_common.hpp"
#include "core/grpc_server/full_res_cache_registry.hpp"

namespace MediaDedup {
namespace Core {

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

    // CL-2: BackupDatabase/GetLatestBackup removed from the proto — they
    // returned success=true while doing NOTHING, and no client called them.


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

} // namespace Core
} // namespace MediaDedup
