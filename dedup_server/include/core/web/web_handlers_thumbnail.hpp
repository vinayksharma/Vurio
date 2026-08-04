#pragma once

#include "config/unified_observable_config.hpp"
#include "filesmanager/disk_cache.hpp"
#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <memory>
#include <mutex>
#include <array>
#include <map>

namespace MediaDedup
{
    class DatabaseManager;
    class ThreadPoolManager;

    /**
     * @brief Handler for GET /api/v1/thumbnails
     *
     * Retrieves or generates thumbnails for image files.
     * Supports ALL image formats including RAW files (ARW, CR2, NEF, etc.) natively
     * via ImageMagick - no transcoding required!
     *
     * Query parameters:
     * - path (required): URL-encoded source file path
     * - size (optional): Thumbnail size (128, 256, 512, 1024), default 256
     *
     * Response:
     * - 200: JPEG image with caching headers
     * - 400: Invalid parameters
     * - 404: Source file not found or not an image
     * - 504: Generation timeout
     * - 500: Server error
     */
    class ThumbnailHandler : public Poco::Net::HTTPRequestHandler
    {
    public:
        ThumbnailHandler(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                         std::shared_ptr<DatabaseManager> database_manager,
                         std::shared_ptr<DiskCache> thumbnail_cache,
                         std::shared_ptr<DiskCache> transcoding_cache,
                         std::shared_ptr<ThreadPoolManager> tpm);

        void handleRequest(Poco::Net::HTTPServerRequest &request,
                           Poco::Net::HTTPServerResponse &response) override;

        bool generateAndCacheThumbnail(const std::string &source_path,
                                       int size,
                                       std::string &cached_path);

        bool getOrCreateErrorThumbnail(int size, std::string &cached_path);

    private:
        std::shared_ptr<UnifiedObservableConfigManager> config_manager_;
        std::shared_ptr<DatabaseManager> database_manager_;
        std::shared_ptr<DiskCache> thumbnail_cache_;
        std::shared_ptr<DiskCache> transcoding_cache_;
        std::shared_ptr<ThreadPoolManager> tpm_;

        // Per-path generation locks to prevent duplicate work
        // CC-6: striped generation locks. The old per-key map erased entries
        // WHILE the caller still held the mutex, letting a second thread mint
        // a fresh mutex for the same key (double generation + racing cache
        // writes) — and every early return leaked an entry forever. A fixed
        // stripe array has no erase, no leak, no race; hash collisions merely
        // serialize the occasional unrelated generation.
        static std::array<std::mutex, 64> generation_stripes_;
        static std::mutex &generationStripe(const std::string &key);

        void streamThumbnailFile(const std::string &file_path,
                                 Poco::Net::HTTPServerResponse &response,
                                 int64_t source_modified_at);

        void sendErrorResponse(Poco::Net::HTTPServerResponse &response,
                               const std::string &error_message,
                               int status_code);
    };

    /**
     * @brief Handler for DELETE /api/v1/thumbnails/cleanup
     *
     * Cleanup stale or orphaned thumbnails.
     * Query parameters:
     * - check_source (optional): If "true", removes thumbnails whose source files don't exist
     *
     * Response:
     * - 200: JSON with removed_count and freed_bytes
     * - 500: Server error
     */
    class ThumbnailCleanupHandler : public Poco::Net::HTTPRequestHandler
    {
    public:
        ThumbnailCleanupHandler(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                                std::shared_ptr<DatabaseManager> database_manager,
                                std::shared_ptr<DiskCache> thumbnail_cache);

        void handleRequest(Poco::Net::HTTPServerRequest &request,
                           Poco::Net::HTTPServerResponse &response) override;

    private:
        std::shared_ptr<UnifiedObservableConfigManager> config_manager_;
        std::shared_ptr<DatabaseManager> database_manager_;
        std::shared_ptr<DiskCache> thumbnail_cache_;

        void sendJsonResponse(Poco::Net::HTTPServerResponse &response,
                              const std::string &json_data,
                              int status_code = 200);

        void sendErrorResponse(Poco::Net::HTTPServerResponse &response,
                               const std::string &error_message,
                               int status_code);
    };

} // namespace MediaDedup
