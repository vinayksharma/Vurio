#pragma once

#include "config/unified_observable_config.hpp"
#include "filesmanager/disk_cache.hpp"
#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <memory>
#include <mutex>
#include <map>

namespace MediaDedup
{
    /**
     * @brief Handler for GET /api/v1/assets/jpeg
     *
     * Returns a JPEG for a given asset path. Always serves from a cache under
     * cache.assetPreview.location. If the source is not JPEG or resizing is requested,
     * it will generate a JPEG into the cache first.
     *
     * Query params:
     * - path (required): URL-encoded source file path
     * - maxWidth (optional): maximum width (pixels). If absent, no resize is applied.
     * - quality (optional): JPEG quality (1-100), default from config
     */
    class AssetPreviewHandler : public Poco::Net::HTTPRequestHandler
    {
    public:
        explicit AssetPreviewHandler(std::shared_ptr<UnifiedObservableConfigManager> config_manager);

        void handleRequest(Poco::Net::HTTPServerRequest &request,
                           Poco::Net::HTTPServerResponse &response) override;

    private:
        std::shared_ptr<UnifiedObservableConfigManager> config_manager_;

        // Per-key generation locks (path+params) to avoid duplicate work
        static std::map<std::string, std::shared_ptr<std::mutex>> generation_locks_;
        static std::mutex locks_mutex_;

        std::shared_ptr<std::mutex> getGenerationLock(const std::string &key);
        void releaseGenerationLock(const std::string &key);

        bool ensureCachedJpeg(const std::string &source_path,
                              int max_width,
                              int quality,
                              std::string &cached_output_path);

        void streamJpegFile(const std::string &file_path,
                            Poco::Net::HTTPServerResponse &response,
                            int64_t source_modified_at);

        void sendJsonError(Poco::Net::HTTPServerResponse &response,
                           const std::string &message,
                           int status_code);

        // Note: No CORS/auth for now; server runs on same machine. Revisit if exposed.
    };
}
