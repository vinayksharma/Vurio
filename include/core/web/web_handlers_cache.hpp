#pragma once

#include "config/unified_observable_config.hpp"
#include "filesmanager/disk_cache.hpp"
#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <memory>

namespace MediaDedup
{
    /**
     * @brief Handler for DELETE /api/v1/cache/{cache_name}/invalidate
     *
     * Invalidates (clears) a disk cache by deleting all files in the cache directory.
     * Only supports DiskCache-based caches.
     *
     * Path parameters:
     * - cache_name (required): Cache name (e.g., "cache.thumbnail", "cache.disk")
     *
     * Supported cache names:
     * - "cache.thumbnail": Thumbnail cache
     * - "cache.disk": Transcoding cache
     *
     * Response:
     * - 200: JSON with status (true if successful, false otherwise)
     * - 400: Invalid cache name
     * - 500: Server error
     */
    class CacheInvalidationHandler : public Poco::Net::HTTPRequestHandler
    {
    public:
        CacheInvalidationHandler(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                                std::shared_ptr<DiskCache> thumbnail_cache,
                                std::shared_ptr<DiskCache> transcoding_cache);

        void handleRequest(Poco::Net::HTTPServerRequest &request,
                           Poco::Net::HTTPServerResponse &response) override;

    private:
        std::shared_ptr<UnifiedObservableConfigManager> config_manager_;
        std::shared_ptr<DiskCache> thumbnail_cache_;
        std::shared_ptr<DiskCache> transcoding_cache_;

        void sendJsonResponse(Poco::Net::HTTPServerResponse &response,
                              const std::string &json_data,
                              int status_code = 200);

        void sendErrorResponse(Poco::Net::HTTPServerResponse &response,
                               const std::string &error_message,
                               int status_code);
    };

} // namespace MediaDedup
