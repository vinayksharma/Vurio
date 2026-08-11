#pragma once

// AR-5: thumbnail / preview / full-resolution media service.
// Moved verbatim out of the former grpc_server_core.cpp god file so the class
// is constructible from tests (which previously documented being unable to
// instantiate the TU-private impls).

#include "core/grpc_server/grpc_service_common.hpp"
#include "core/grpc_server/full_res_cache_registry.hpp"

namespace MediaDedup {
namespace Core {

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
                                if (!PreviewCacheOps::upsertPreview(
                                        *db_copy, PreviewCacheOps::makeRecord(path_copy, final_path))) {
                                    // Without the row nothing can ever find this
                                    // preview, and announcing it ready would send
                                    // the client straight back for another full
                                    // transcode. Discard and report transient.
                                    xcache_copy->deleteFromCacheImmediately(final_path);
                                    MediaDedup::MediaReadyBus::instance().publish(
                                        {path_copy, "preview", false, "Preview generation failed (transient server error)"});
                                    return;
                                }

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

} // namespace Core
} // namespace MediaDedup
