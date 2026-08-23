#include "orchestration/integrity_check_service.hpp"

#include "core/media_backfill.hpp"
#include "database/database_manager.hpp"
#include "database/duplicate_groups_ops.hpp"
#include "database/scanned_files_ops.hpp"
#include "orchestration/duplicate_finder.hpp"
#include "orchestration/scheduler_service.hpp"
#include "orchestration/thread_pool_manager.hpp"

#include <Poco/Data/RecordSet.h>
#include <Poco/Data/Session.h>
#include <Poco/Data/Statement.h>
#include <Poco/Logger.h>
#include <chrono>
#include <filesystem>
#include <thread>

namespace MediaDedup
{
    namespace Orchestration
    {
        IntegrityCheckService::State IntegrityCheckService::state_;
        std::mutex IntegrityCheckService::state_mutex_;

        bool IntegrityCheckService::start(std::shared_ptr<DatabaseManager> db,
                                          std::shared_ptr<UnifiedObservableConfigManager> cfg,
                                          std::shared_ptr<SchedulerService> scheduler,
                                          std::shared_ptr<ThreadPoolManager> tpm,
                                          std::string &out_error)
        {
            Poco::Logger &logger = Poco::Logger::get("IntegrityCheckService");

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

            // Detached but lifetime-safe (CC-5): every argument is a REAL
            // shared_ptr (DatabaseManager ownership is genuinely shared since
            // CC-5), so the thread keeps its dependencies alive on its own.
            // is_running gates re-entry; the catch handlers in run()
            // guarantee it resets.
            std::thread t(run, db, cfg, scheduler, tpm);
            t.detach();

            return true;
        }

        IntegrityCheckStatusSnapshot IntegrityCheckService::status()
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

        void IntegrityCheckService::run(
            std::shared_ptr<DatabaseManager> db,
            std::shared_ptr<UnifiedObservableConfigManager> cfg,
            std::shared_ptr<SchedulerService> scheduler,
            std::shared_ptr<ThreadPoolManager> tpm)
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
                // CC-5: bounded — the old unbounded 5 Hz loop spun forever if
                // anything wedged the pool, on a thread nobody could stop.
                {
                    const auto drain_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
                    while (tpm->getStatus().runningTotal > 0 &&
                           std::chrono::steady_clock::now() < drain_deadline) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    }
                    if (tpm->getStatus().runningTotal > 0)
                        logger.warning("ThreadPoolManager did not fully drain within 60 s; proceeding with the check anyway.");
                    else
                        logger.information("All background threads halted successfully.");
                }

                // Phase 2: Sweep Scanned Files
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    state_.current_phase = "Checking physical file existence...";
                }
                logger.information("Fetching all scanned files...");
                auto all_files = ScannedFilesOps::listAll(*db);
                
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
                        auto groups = DuplicateGroupsOps::getGroupsForFile(*db, file.id, "EMBEDDING");
                        for (const auto& g : groups) {
                            if (g.representative_file_id == file.id) {
                                DuplicateFinder::promoteNextBestRepresentative(*db, g.id, file.id, purity_str);
                            }
                        }

                        // Completely wipe
                        if (ScannedFilesOps::completelyRemoveById(*db, file.id)) {
                            std::lock_guard<std::mutex> lock(state_mutex_);
                            state_.files_removed++;
                        }

                        // Check remaining groups size and sync them
                        for (const auto& g : groups) {
                            int actual_count = DuplicateGroupsOps::syncGroupMemberCount(*db, g.id);
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

                // Phase 6: RAW EXIF refresh. Rows written by the pre-fix
                // LibRaw reader lack the searchable EXIF columns (ISO,
                // aperture, shutter, GPS, ...); the sweep refills them with
                // metadata-only reads — no decode, no thumbnails. The real
                // work lives in MediaBackfill (server-only TU) behind a
                // hook, so test binaries compiling this file don't drag in
                // LibRaw/ImageMagick. (The former thumbnail/preview
                // backfill is gone: those are generated on demand by
                // design, and pre-generating them here fought that design.)
                //
                // The sweep reports its own file counts through the
                // progress callback so the dialog shows the sweep's real
                // position, not the stale numbers of an earlier phase.
                {
                    {
                        std::lock_guard<std::mutex> lock(state_mutex_);
                        state_.current_phase = "Refreshing RAW photo metadata...";
                        state_.total_files = 0;
                        state_.processed_files = 0;
                    }
                    MediaBackfillHook::run(db, [](std::size_t done, std::size_t total) {
                        std::lock_guard<std::mutex> lock(state_mutex_);
                        state_.total_files = total;
                        state_.processed_files = done;
                        state_.progress = total ? static_cast<int>((done * 100) / total) : 100;
                    });
                }

                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    state_.current_phase = "Complete";
                    state_.progress = 100;
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
            } catch (...) {
                // CC-5: without this, a non-std exception escaping the thread
                // function called std::terminate — and even short of that,
                // is_running stayed true forever (blocking future checks) with
                // the scheduler left paused.
                logger.error("Unknown exception during database integrity check");
                try { scheduler->resume(); } catch (...) {}
                std::lock_guard<std::mutex> lock(state_mutex_);
                state_.is_running = false;
                state_.current_phase = "Failed: unknown error";
            }
        }
    } // namespace Orchestration
} // namespace MediaDedup
