#pragma once

#include <functional>
#include <memory>

namespace Poco
{
    class Logger;
}

namespace MediaDedup
{
    class DatabaseManager;
    class UnifiedObservableConfigManager;
    class ThreadPoolManager;

    /**
     * @brief Seam between the database integrity check and the
     * thumbnail/preview backfill.
     *
     * The backfill needs the whole media-generation stack (ThumbnailHandler,
     * TranscodingPipeline, DiskCache, ImageMagick/LibRaw); linking that into
     * every test binary that compiles web_handlers_duplicates.cpp is not
     * acceptable. The integrity check therefore calls through this hook, and
     * the real implementation (media_backfill.cpp, linked only into the
     * server) registers itself at startup. Unregistered (tests) → no-op.
     */
    class MediaBackfillHook
    {
    public:
        using Fn = std::function<void(std::shared_ptr<DatabaseManager>,
                                      std::shared_ptr<UnifiedObservableConfigManager>,
                                      std::shared_ptr<ThreadPoolManager>)>;

        static void install(Fn fn);

        /// Runs the installed backfill; silently does nothing when none is installed.
        static void run(std::shared_ptr<DatabaseManager> db,
                        std::shared_ptr<UnifiedObservableConfigManager> cfg,
                        std::shared_ptr<ThreadPoolManager> tpm);
    };

    class DiskCache;

    /**
     * @brief The real backfill: enqueues regeneration for processed files
     * missing a thumbnail_cache row or a needed preview. Linked into the
     * server binary only; installed onto MediaBackfillHook at startup.
     *
     * MP-2: installHook captures the initializer's REAL DiskCache instances —
     * the backfill previously constructed a second pair over the same
     * directories (split-brain size accounting, void in-use protection).
     */
    class MediaBackfill
    {
    public:
        static void installHook(std::shared_ptr<DiskCache> thumbnail_cache,
                                std::shared_ptr<DiskCache> transcoding_cache);

        static void run(std::shared_ptr<DatabaseManager> db,
                        std::shared_ptr<UnifiedObservableConfigManager> cfg,
                        std::shared_ptr<ThreadPoolManager> tpm,
                        std::shared_ptr<DiskCache> thumbnail_cache,
                        std::shared_ptr<DiskCache> transcoding_cache);

        /**
         * @brief RAW-EXIF: one-shot sweep refilling the rich EXIF columns
         * for RAW rows written by the pre-fix LibRaw reader (which captured
         * only camera/datetime/dimensions). Selector: RAW extension with
         * exif_camera set but exif_make NULL — a successful re-read fills
         * exif_make, so the sweep converges. Runs inline (LibRaw
         * metadata-only opens, no demosaic) on the integrity-check thread.
         */
        static void backfillRawExif(DatabaseManager &db, Poco::Logger &logger);
    };

} // namespace MediaDedup
