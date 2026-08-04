#pragma once

#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <memory>
#include <string>
#include <vector>
#include <filesystem>
#include "config/unified_observable_config.hpp"
#include "services/eject_service.hpp"

namespace MediaDedup
{
    class DatabaseManager;
    class UnifiedObservableConfigManager;
    class ThreadPoolManager;
    namespace Orchestration {
        class SchedulerService;
    }
}

namespace MediaDedupServer
{
    namespace Core
    {
        // AR-2: the eject/restore/purge logic (VerifiedMove*, BulkOpResult,
        // bulkEject/bulkUndoEject, category ejects) moved to
        // services/eject_service.hpp. Aliases keep this legacy surface
        // compiling until AR-3 deletes it.
        using MediaDedup::Services::VerifiedMoveResult;
        using MediaDedup::Services::copyFileVerified;
        using MediaDedup::Services::moveFileVerified;
        using MediaDedup::Services::BulkOpResult;
        using MediaDedup::Services::bulkEject;
        using MediaDedup::Services::bulkUndoEject;

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
         * all processed artifacts (embeddings).
         *
         * Query Parameters:
         * - mode (optional): Specific mode to reset (only "EMBEDDING" is supported)
         *                    Omit to reset EMBEDDING mode (default)
         *
         * What gets cleared:
         * - duplicate_groups table (for EMBEDDING mode)
         * - duplicate_members table (for EMBEDDING mode)
         * - duplicate_processing_checkpoint (reset to 0)
         *
         * What is PRESERVED:
         * - scanned_files table (file scan data)
         * - image_artifacts table (embeddings)
         *
         * Response Format:
         * {
         *   "success": true
         * }
         */
        class ResetDuplicatesHandler : public Poco::Net::HTTPRequestHandler
        {
        public:
            explicit ResetDuplicatesHandler(std::shared_ptr<MediaDedup::DatabaseManager> db, std::shared_ptr<MediaDedup::Orchestration::SchedulerService> scheduler_service);

            void handleRequest(Poco::Net::HTTPServerRequest &request,
                               Poco::Net::HTTPServerResponse &response) override;

        private:
            std::shared_ptr<MediaDedup::DatabaseManager> database_manager_;
            std::shared_ptr<MediaDedup::Orchestration::SchedulerService> scheduler_service_;
        };

        /**
         * @brief Handler for POST /api/v1/duplicates/eject
         *
         * Ejects duplicates by copying them to an ejected_<uuid> folder at their registered root location,
         * verified via SHA1 checksum, and then explicitly replacing their original path in the DB
         * while deleting the original source file.
         */
        class EjectDuplicatesHandler : public Poco::Net::HTTPRequestHandler
        {
        public:
            explicit EjectDuplicatesHandler(std::shared_ptr<MediaDedup::DatabaseManager> db);

            void handleRequest(Poco::Net::HTTPServerRequest &request,
                               Poco::Net::HTTPServerResponse &response) override;

        private:
            std::shared_ptr<MediaDedup::DatabaseManager> database_manager_;
        };

        /**
         * @brief Handler for POST /api/v1/duplicates/undo-eject
         *
         * Restores ejected images to their original location.
         */
        class UndoEjectDuplicatesHandler : public Poco::Net::HTTPRequestHandler
        {
        public:
            explicit UndoEjectDuplicatesHandler(std::shared_ptr<MediaDedup::DatabaseManager> db);

            void handleRequest(Poco::Net::HTTPServerRequest &request,
                               Poco::Net::HTTPServerResponse &response) override;

        private:
            std::shared_ptr<MediaDedup::DatabaseManager> database_manager_;
        };

        /**
         * @brief Handler for POST /api/v1/duplicates/purge-ejected-errored
         *
         * Completely removes all ejected and errored files from the database while preserving duplicate groups.
         */
        class PurgeEjectedErroredHandler : public Poco::Net::HTTPRequestHandler
        {
        public:
            explicit PurgeEjectedErroredHandler(std::shared_ptr<MediaDedup::DatabaseManager> db, std::shared_ptr<MediaDedup::UnifiedObservableConfigManager> cfg);

            void handleRequest(Poco::Net::HTTPServerRequest &request,
                               Poco::Net::HTTPServerResponse &response) override;

        private:
            std::shared_ptr<MediaDedup::DatabaseManager> database_manager_;
            std::shared_ptr<MediaDedup::UnifiedObservableConfigManager> config_manager_;
        };

        // AR-1: IntegrityCheckState/Snapshot and the runner moved to
        // MediaDedup::Orchestration::IntegrityCheckService.
        class DatabaseIntegrityCheckHandler : public Poco::Net::HTTPRequestHandler
        {
        public:
            explicit DatabaseIntegrityCheckHandler(
                std::shared_ptr<MediaDedup::DatabaseManager> db, 
                std::shared_ptr<MediaDedup::UnifiedObservableConfigManager> cfg,
                std::shared_ptr<MediaDedup::Orchestration::SchedulerService> scheduler,
                std::shared_ptr<MediaDedup::ThreadPoolManager> tpm);

            void handleRequest(Poco::Net::HTTPServerRequest &request,
                               Poco::Net::HTTPServerResponse &response) override;

        private:
            std::shared_ptr<MediaDedup::DatabaseManager> database_manager_;
            std::shared_ptr<MediaDedup::UnifiedObservableConfigManager> config_manager_;
            std::shared_ptr<MediaDedup::Orchestration::SchedulerService> scheduler_service_;
            std::shared_ptr<MediaDedup::ThreadPoolManager> tpm_;

            void handleStart(Poco::Net::HTTPServerResponse &response);
            void handleStatus(Poco::Net::HTTPServerResponse &response);
        };

        class BackupDatabaseHandler : public Poco::Net::HTTPRequestHandler
        {
        public:
            explicit BackupDatabaseHandler(std::shared_ptr<MediaDedup::DatabaseManager> db);

            void handleRequest(Poco::Net::HTTPServerRequest &request,
                               Poco::Net::HTTPServerResponse &response) override;

        private:
            std::shared_ptr<MediaDedup::DatabaseManager> database_manager_;
        };

        class GetLatestBackupHandler : public Poco::Net::HTTPRequestHandler
        {
        public:
            explicit GetLatestBackupHandler(std::shared_ptr<MediaDedup::DatabaseManager> db);

            void handleRequest(Poco::Net::HTTPServerRequest &request,
                               Poco::Net::HTTPServerResponse &response) override;

        private:
            std::shared_ptr<MediaDedup::DatabaseManager> database_manager_;
        };

    } // namespace Core
} // namespace MediaDedupServer
