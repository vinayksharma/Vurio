#pragma once

#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <memory>

namespace MediaDedup
{
    class DatabaseManager;
}

namespace MediaDedupServer
{
    namespace Core
    {
        /**
         * @brief Handler for GET /api/v1/duplicates/groups
         *
         * Returns paginated list of duplicate groups with their representative
         * and candidate files (all as fully qualified paths).
         *
         * Query Parameters:
         * - start: Starting index (default: 0)
         * - limit: Number of groups to return (default: 100, max: 1000)
         *
         * Response Format:
         * {
         *   "groups": [
         *     {
         *       "id": 1,
         *       "mode": "FAST",
         *       "representative": "/full/path/to/file.jpg",
         *       "candidates": ["/full/path/to/dup1.jpg", "/full/path/to/dup2.jpg"],
         *       "similarity_threshold": 0.95,
         *       "member_count": 3,
         *       "created_at": "2025-10-18 10:30:45",
         *       "updated_at": "2025-10-18 10:31:20"
         *     }
         *   ],
         *   "total_count": 1523,
         *   "start": 0,
         *   "end": 100,
         *   "returned": 100
         * }
         */
        class DuplicateGroupsHandler : public Poco::Net::HTTPRequestHandler
        {
        public:
            explicit DuplicateGroupsHandler(std::shared_ptr<MediaDedup::DatabaseManager> db);

            void handleRequest(Poco::Net::HTTPServerRequest &request,
                               Poco::Net::HTTPServerResponse &response) override;

        private:
            std::shared_ptr<MediaDedup::DatabaseManager> database_manager_;
        };

    } // namespace Core
} // namespace MediaDedupServer
