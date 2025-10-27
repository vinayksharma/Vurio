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
         *       "mode": "EMBEDDING",
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

        /**
         * @brief Handler for DELETE /api/v1/duplicates/reset
         *
         * Resets duplicate detection by clearing groups and checkpoints while preserving
         * all processed artifacts (phash, features, embeddings).
         *
         * Query Parameters:
         * - mode (optional): Specific mode to reset (EMBEDDING - single mode operation)
         *                    Omit to reset ALL modes
         *
         * What gets cleared:
         * - duplicate_groups table (for specified mode(s))
         * - duplicate_members table (for specified mode(s))
         * - duplicate_processing_checkpoint (reset to 0)
         *
         * What is PRESERVED:
         * - scanned_files table (file scan data)
         * - image_artifacts table (phash, features, embeddings)
         *
         * Response Format:
         * {
         *   "success": true
         * }
         */
        class ResetDuplicatesHandler : public Poco::Net::HTTPRequestHandler
        {
        public:
            explicit ResetDuplicatesHandler(std::shared_ptr<MediaDedup::DatabaseManager> db);

            void handleRequest(Poco::Net::HTTPServerRequest &request,
                               Poco::Net::HTTPServerResponse &response) override;

        private:
            std::shared_ptr<MediaDedup::DatabaseManager> database_manager_;
        };

    } // namespace Core
} // namespace MediaDedupServer
