#include "core/media_backfill.hpp"

#include "core/media_generation_failures.hpp"
#include "core/media_ready_bus.hpp"
#include "core/web/web_handlers_thumbnail.hpp"
#include "database/database_manager.hpp"
#include "database/preview_cache_ops.hpp"
#include "filesmanager/disk_cache.hpp"
#include "media_processors/image/backends/transcoding_pipeline.hpp"
#include "orchestration/thread_pool_manager.hpp"

#include <Poco/Data/RecordSet.h>
#include <Poco/Data/Session.h>
#include <Poco/Data/Statement.h>
#include <Poco/Logger.h>
#include <chrono>
#include <filesystem>
#include <vector>

namespace MediaDedup
{
    void MediaBackfill::installHook()
    {
        MediaBackfillHook::install([](std::shared_ptr<DatabaseManager> db,
                                      std::shared_ptr<UnifiedObservableConfigManager> cfg,
                                      std::shared_ptr<ThreadPoolManager> tpm) {
            MediaBackfill::run(std::move(db), std::move(cfg), std::move(tpm));
        });
    }

    void MediaBackfill::run(std::shared_ptr<DatabaseManager> db,
                            std::shared_ptr<UnifiedObservableConfigManager> cfg,
                            std::shared_ptr<ThreadPoolManager> tpm)
    {
        Poco::Logger &logger = Poco::Logger::get("MediaBackfill");
        if (!db || !cfg || !tpm)
            return;

        try
        {
            std::vector<std::string> missing_thumbs;
            std::vector<std::string> preview_candidates;
            {
                auto lease = db->acquireSessionLease();
                Poco::Data::Session &sess = lease.get();

                Poco::Data::Statement sel_thumbs(sess);
                sel_thumbs << "SELECT file_path FROM scanned_files WHERE processed >= 2 "
                              "AND file_path NOT IN (SELECT source_path FROM thumbnail_cache) LIMIT 5000";
                Poco::Data::RecordSet rs_thumbs(sel_thumbs);
                for (bool more = rs_thumbs.moveFirst(); more; more = rs_thumbs.moveNext())
                {
                    missing_thumbs.push_back(rs_thumbs[0].convert<std::string>());
                }

                Poco::Data::Statement sel_previews(sess);
                sel_previews << "SELECT file_path FROM scanned_files WHERE processed >= 2 "
                                "AND file_path NOT IN (SELECT source_path FROM preview_cache) LIMIT 20000";
                Poco::Data::RecordSet rs_previews(sel_previews);
                for (bool more = rs_previews.moveFirst(); more; more = rs_previews.moveNext())
                {
                    preview_candidates.push_back(rs_previews[0].convert<std::string>());
                }
            }

            if (missing_thumbs.empty() && preview_candidates.empty())
                return;

            // Same cache directories the server initializer builds
            // (config-driven), constructed locally because the integrity
            // handler isn't plumbed with the instances.
            auto backfill_thumb_cache = std::make_shared<DiskCache>(cfg, "cache.thumbnail");
            auto backfill_trans_cache = std::make_shared<DiskCache>(cfg, "cache.disk");
            if (!backfill_thumb_cache->initialize() || !backfill_trans_cache->initialize())
            {
                logger.warning("Backfill skipped: disk caches failed to initialize.");
                return;
            }

            int thumb_size = cfg->getPropertyValue<int>("thumbnail.pipeline.size", 256);
            int enqueued_thumbs = 0;
            for (const auto &path : missing_thumbs)
            {
                if (!std::filesystem::exists(path))
                    continue;
                tpm->submit("thumbnail_generator", [cfg, db, backfill_thumb_cache, backfill_trans_cache, tpm, path, thumb_size]() {
                    ThumbnailHandler temp_handler(cfg, db, backfill_thumb_cache, backfill_trans_cache, tpm);
                    std::string cached_path;
                    if (temp_handler.generateAndCacheThumbnail(path, thumb_size, cached_path))
                    {
                        MediaGenerationFailures::instance().recordSuccess(path, "thumbnail");
                        MediaReadyBus::instance().publish({path, "thumbnail", true, ""});
                    }
                    else
                    {
                        MediaGenerationFailures::instance().recordFailure(
                            path, "thumbnail", "Thumbnail backfill failed (unsupported or corrupted file)");
                    }
                }, path);
                enqueued_thumbs++;
            }

            int enqueued_previews = 0;
            for (const auto &path : preview_candidates)
            {
                if (!std::filesystem::exists(path))
                    continue;
                if (!TranscodingPipeline::NeedsQhdPreviewCache(path, cfg))
                    continue;
                tpm->submit("preview_generator", [cfg, db, backfill_trans_cache, path]() {
                    TranscodingConfig transcode_config = TranscodingPipeline::GetConfigFromManager(cfg);
                    if (!transcode_config.enabled)
                        return;
                    std::string temp_dir = std::filesystem::temp_directory_path().string();
                    std::string temp_path;
                    if (TranscodingPipeline::TranscodeToFile(path, temp_path, transcode_config, temp_dir))
                    {
                        std::string final_path;
                        if (backfill_trans_cache->moveToCache(temp_path, final_path))
                        {
                            PreviewCacheRecord record;
                            record.source_path = path;
                            record.cached_path = final_path;
                            try
                            {
                                auto ftime = std::filesystem::last_write_time(path);
                                auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                                    ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
                                record.source_modified_at = std::chrono::system_clock::to_time_t(sctp);
                            }
                            catch (...)
                            {
                                record.source_modified_at = 0;
                            }
                            try
                            {
                                record.file_size_bytes = std::filesystem::file_size(final_path);
                            }
                            catch (...)
                            {
                                record.file_size_bytes = 0;
                            }
                            int64_t now_ts = std::chrono::duration_cast<std::chrono::seconds>(
                                                 std::chrono::system_clock::now().time_since_epoch())
                                                 .count();
                            record.created_at = now_ts;
                            record.last_accessed_at = now_ts;
                            PreviewCacheOps::upsertPreview(*db, record);
                            MediaGenerationFailures::instance().recordSuccess(path, "preview");
                            MediaReadyBus::instance().publish({path, "preview", true, ""});
                        }
                        else
                        {
                            std::error_code ec;
                            std::filesystem::remove(temp_path, ec);
                        }
                    }
                    else
                    {
                        MediaGenerationFailures::instance().recordFailure(
                            path, "preview", "Preview backfill failed (unsupported or corrupted file)");
                    }
                }, path);
                enqueued_previews++;
            }

            logger.information("Backfill enqueued %d missing thumbnails and %d missing previews.",
                               enqueued_thumbs, enqueued_previews);
        }
        catch (const std::exception &e)
        {
            logger.warning("Thumbnail/preview backfill failed: %s", std::string(e.what()));
        }
    }

} // namespace MediaDedup
