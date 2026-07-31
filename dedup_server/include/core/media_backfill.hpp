#pragma once

#include <functional>
#include <memory>

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

    /**
     * @brief The real backfill: enqueues regeneration for processed files
     * missing a thumbnail_cache row or a needed preview. Linked into the
     * server binary only; installed onto MediaBackfillHook at startup.
     */
    class MediaBackfill
    {
    public:
        static void installHook();

        static void run(std::shared_ptr<DatabaseManager> db,
                        std::shared_ptr<UnifiedObservableConfigManager> cfg,
                        std::shared_ptr<ThreadPoolManager> tpm);
    };

} // namespace MediaDedup
