#pragma once

// AR-5: media locations + day/timeline service.
// Moved verbatim out of the former grpc_server_core.cpp god file so the class
// is constructible from tests (which previously documented being unable to
// instantiate the TU-private impls).

#include "core/grpc_server/grpc_service_common.hpp"
// repairProgress() is called inline below, so the full type is needed.
#include "orchestration/duplicate_finder.hpp"

namespace MediaDedup {
namespace Core {

class MediaLocationsServiceImpl final : public vurio::dedup::MediaLocationsService::Service {
    std::shared_ptr<FilesService> files_;
    std::shared_ptr<ThreadPoolManager> tpm_;
    std::shared_ptr<DatabaseManager> db_owner_; // CC-5
    DatabaseManager* db_;
    std::shared_ptr<MediaDedup::Orchestration::SchedulerService> scheduler_;
    // Read only for repairProgress() — in-memory atomics, no DB work on
    // the 2 s status poll. Optional: null in tests and if the finder is disabled.
    std::shared_ptr<MediaDedup::Orchestration::DuplicateFinder> duplicate_finder_;
public:
    explicit MediaLocationsServiceImpl(std::shared_ptr<FilesService> f, std::shared_ptr<ThreadPoolManager> tpm, DatabaseManager* db, std::shared_ptr<MediaDedup::Orchestration::SchedulerService> scheduler,
                                       std::shared_ptr<MediaDedup::Orchestration::DuplicateFinder> duplicate_finder = nullptr)
        : files_(std::move(f)), tpm_(std::move(tpm)), db_(db), scheduler_(std::move(scheduler)),
          duplicate_finder_(std::move(duplicate_finder)) {}

    grpc::Status GetStatus(grpc::ServerContext* context, const vurio::dedup::GetStatusRequest* request,
                           vurio::dedup::GetStatusResponse* reply) override {
        auto locations = files_->listMediaLocations();
        for (const auto& pair : locations) {
            reply->add_registered_dirs(pair.second);
        }
        
        if (db_) {
            // DB-7: one aggregated query + one lease for all five
            // scanned_files counts (this poll runs every 2 s). DISC-1: the
            // discovery fraction comes from the per-file flag, so no checkpoint
            // read is needed here any more.
            MediaDedup::ScannedFilesOps::ScanStatusCounts counts;
            MediaDedup::ScannedFilesOps::getStatusCounts(*db_, counts);
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
        if (duplicate_finder_) {
            auto repair = duplicate_finder_->repairProgress();
            reply->set_repair_total(repair.total);
            reply->set_repair_completed(repair.completed);
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

} // namespace Core
} // namespace MediaDedup
