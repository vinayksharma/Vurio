#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

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

        private:
            std::shared_ptr<UnifiedObservableConfigManager> cfg_;
            std::shared_ptr<DatabaseManager> db_;
            std::shared_ptr<FilesService> filesService_;
            std::shared_ptr<MediaProcessor> mediaProcessor_;

            std::mutex runMutex_;
            bool running_ = false;

            void onFileEmitted(const std::string &root,
                               const MediaDedup::Files::FileRecord &rec,
                               std::unordered_map<std::string, MediaDedup::ScannedFileRow> &index);
        };
    } // namespace Orchestration
} // namespace MediaDedup
