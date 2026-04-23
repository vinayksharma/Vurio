#pragma once

#include "config/unified_observable_config.hpp"
#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <memory>

namespace MediaDedup
{
    class DatabaseManager;

    /**
     * @brief Handler for GET /api/v1/preview
     *
     * Retrieves full-resolution transcoded previews for RAW files.
     *
     * Query parameters:
     * - path (required): URL-encoded source file path
     *
     * Response:
     * - 200: JPEG image with caching headers
     * - 400: Invalid parameters
     * - 404: Preview not found
     * - 500: Server error
     */
    class PreviewHandler : public Poco::Net::HTTPRequestHandler
    {
    public:
        PreviewHandler(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                       std::shared_ptr<DatabaseManager> database_manager);

        void handleRequest(Poco::Net::HTTPServerRequest &request,
                           Poco::Net::HTTPServerResponse &response) override;

    private:
        std::shared_ptr<UnifiedObservableConfigManager> config_manager_;
        std::shared_ptr<DatabaseManager> database_manager_;

        void streamPreviewFile(const std::string &file_path,
                               Poco::Net::HTTPServerResponse &response,
                               int64_t source_modified_at);

        void sendErrorResponse(Poco::Net::HTTPServerResponse &response,
                               const std::string &error_message,
                               int status_code);
    };

} // namespace MediaDedup
