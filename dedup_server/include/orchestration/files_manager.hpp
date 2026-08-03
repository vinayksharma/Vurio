#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <atomic>

#include "config/unified_observable_config.hpp"
#include "database/database_manager.hpp"
#include "filesmanager/files_service.hpp"
#include "filesmanager/file_scanner.hpp"
#include "database/scanned_files_ops.hpp"
#include "media_processors/media_processor.hpp"

namespace MediaDedup
{
    namespace Orchestration
    {
        class FilesManager
        {
        public:
            FilesManager(std::shared_ptr<UnifiedObservableConfigManager> cfg,
                         std::shared_ptr<DatabaseManager> db,
                         std::shared_ptr<FilesService> filesService,
                         std::shared_ptr<MediaProcessor> mediaProcessor)
                : cfg_(std::move(cfg)), db_(std::move(db)), filesService_(std::move(filesService)), mediaProcessor_(std::move(mediaProcessor)) {}

            void initialize();
            void runOnce();
            void triggerScanNow();

            // Scan status information
            bool isScanInProgress() const;
            std::string getCurrentScanThreadId() const;
            std::chrono::milliseconds getScanDuration() const;

            void abortCurrentScan();

            std::shared_ptr<FilesService> getFilesService() const { return filesService_; }

        private:
            std::shared_ptr<UnifiedObservableConfigManager> cfg_;
            std::shared_ptr<DatabaseManager> db_;
            std::shared_ptr<FilesService> filesService_;
            std::shared_ptr<MediaProcessor> mediaProcessor_;

            std::mutex runMutex_;
            bool running_ = false;
            std::atomic<bool> abortScan_{false};
            std::atomic<bool> pendingReRun_{false};

            // DB-7: new-file rows accumulated during a scan pass and written
            // in one transaction per batch (one WAL commit instead of one per
            // file). Touched only from the scan thread (the scanner invokes
            // the callback synchronously), flushed per batch / per root / on
            // scan exit.
            std::vector<ScannedFileRow> pending_upserts_;
            static constexpr std::size_t kUpsertBatchSize = 500;
            void flushPendingUpserts();

            void onFileEmitted(const std::string &root,
                               const MediaDedup::Files::FileRecord &rec);
        };
    } // namespace Orchestration
} // namespace MediaDedup
