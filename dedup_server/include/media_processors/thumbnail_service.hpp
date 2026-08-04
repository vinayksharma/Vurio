#pragma once

#include <array>
#include <memory>
#include <mutex>
#include <string>

namespace MediaDedup
{
    class DatabaseManager;
    class DiskCache;
    class UnifiedObservableConfigManager;

    /**
     * @brief AR-1: on-demand thumbnail generation, shared by every surface.
     *
     * Extracted from the HTTP ThumbnailHandler: the gRPC service and the
     * media backfill used to construct a full HTTP request handler inside a
     * TPM task just to call this logic — an upward edge from the gRPC layer
     * into core/web. The HTTP handler now delegates here too.
     *
     * Generates a thumbnail blob (ThumbnailGenerator), stores it in the
     * thumbnail DiskCache, and upserts the thumbnail_cache row. Duplicate
     * generations of the same source+size are serialized by striped locks
     * shared process-wide (CC-6).
     */
    class ThumbnailService
    {
    public:
        ThumbnailService(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                         std::shared_ptr<DatabaseManager> database_manager,
                         std::shared_ptr<DiskCache> thumbnail_cache);

        /**
         * @brief Why a generation attempt failed.
         *
         * Callers feed the MediaGenerationFailures negative cache — which
         * permanently brands a file "unsupported or corrupted" after a few
         * strikes — so the distinction matters: only Decode failures are
         * evidence about the FILE. Infrastructure failures (source missing —
         * possibly a disconnected volume — cache write errors, internal
         * exceptions) are transient server conditions and must stay retryable.
         */
        enum class GenerationFailure
        {
            None,
            Decode,        ///< the file itself could not be decoded
            Infrastructure ///< server-side trouble unrelated to file validity
        };

        /**
         * @brief Generate (or reuse a concurrently generated) thumbnail.
         * @param source_path Source image path
         * @param size Cached-thumbnail size (128/256/512/1024)
         * @param cached_path Output: path of the cached thumbnail file
         * @param failure Optional output: failure classification (None on success)
         * @return true on success
         */
        bool generateAndCacheThumbnail(const std::string &source_path,
                                       int size,
                                       std::string &cached_path,
                                       GenerationFailure *failure = nullptr);

    private:
        std::shared_ptr<UnifiedObservableConfigManager> config_manager_;
        std::shared_ptr<DatabaseManager> database_manager_;
        std::shared_ptr<DiskCache> thumbnail_cache_;

        // CC-6: striped generation locks, process-wide so every surface
        // (HTTP, gRPC, backfill) serializes duplicate work on the same key.
        static std::array<std::mutex, 64> generation_stripes_;
        static std::mutex &generationStripe(const std::string &key);
    };

} // namespace MediaDedup
