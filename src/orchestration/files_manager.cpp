#include "orchestration/files_manager.hpp"
#include <Poco/Logger.h>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace MediaDedup::Orchestration
{
    using namespace MediaDedup::Files;
    using nlohmann::json;

    void FilesManager::initialize()
    {
        // Ensure table exists
        Poco::Logger &logger = Poco::Logger::get("FilesManager");
        logger.information("Initializing FilesManager: ensuring scanned_files table exists");
        if (!ScannedFilesOps::ensureTable(*db_))
        {
            logger.warning("Failed to ensure scanned_files table exists (continuing)");
        }
    }

    void FilesManager::triggerScanNow()
    {
        runOnce();
    }

    void FilesManager::onFileEmitted(const std::string &root,
                                     const FileRecord &rec,
                                     std::unordered_map<std::string, MediaDedup::ScannedFileRow> &index)
    {
        Poco::Logger &logger = Poco::Logger::get("FilesManager");
        if (rec.hasError())
        {
            logger.warning("Scan error on %s: %s", rec.fullPath, rec.errorMessage);
            // Persist brief error info if row exists; otherwise skip creating new rows for errors only
            auto it = index.find(rec.fullPath);
            if (it != index.end())
            {
                // store error info in metadata JSON (append fields) — simple approach
                json meta;
                try
                {
                    meta = json::parse(it->second.file_metadata);
                }
                catch (...)
                {
                    meta = json::object();
                }
                meta["lastErrorCode"] = to_string(rec.error);
                meta["lastErrorMessage"] = rec.errorMessage;
                meta["lastErrorErrno"] = rec.platformErrno;
                ScannedFilesOps::updateMetadata(*db_, rec.fullPath, meta.dump());
            }
            return;
        }

        // Success path
        // Build metadata JSON
        json meta;
        meta["sizeBytes"] = rec.fileSizeBytes;
        meta["isHidden"] = rec.isHidden;
        meta["symlinkTarget"] = rec.symlinkTarget;
        meta["deviceId"] = rec.deviceId;
        meta["inode"] = rec.inode;

        // Relative path
        std::string relativeStr;
        try
        {
            relativeStr = fs::path(rec.fullPath).lexically_relative(fs::path(root)).string();
        }
        catch (...)
        {
        }

        // Determine share name best-effort (stub empty)
        std::string shareName;

        // Prepare row
        ScannedFileRow row;
        row.file_path = rec.fullPath;
        row.relative_path = relativeStr;
        row.share_name = shareName;
        row.file_name = rec.fileName;
        row.file_metadata = meta.dump();
        row.is_network_file = rec.isShareMapped; // best-effort

        auto found = index.find(rec.fullPath);
        const bool exists = (found != index.end());
        bool changed = false;
        if (exists)
        {
            // compare minimal fields: size/time/attrs — simplified: if metadata string differs after update, treat as changed
            changed = (found->second.file_metadata != row.file_metadata);
        }

        // FilesManager only sets processed_* to 0 on new/changed
        if (!exists || changed)
        {
            logger.information(std::string(!exists ? "Discovered new file: " : "Detected changed file: ") + rec.fullPath);
            row.processed_fast = 0;
            row.processed_balanced = 0;
            row.processed_quality = 0;
        }
        else
        {
            logger.information(std::string("Unchanged file (keeping processed states): ") + rec.fullPath);
            // keep existing states
            row.processed_fast = found->second.processed_fast;
            row.processed_balanced = found->second.processed_balanced;
            row.processed_quality = found->second.processed_quality;
        }

        ScannedFilesOps::upsert(*db_, row);
        index[row.file_path] = row;
    }

    void FilesManager::runOnce()
    {
        std::unique_lock<std::mutex> lk(runMutex_, std::try_to_lock);
        if (!lk.owns_lock() || running_)
            return;
        running_ = true;
        Poco::Logger &logger = Poco::Logger::get("FilesManager");
        logger.information("FilesManager run started");

        try
        {
            // Build in-memory index
            logger.information("Building in-memory index from scanned_files");
            std::unordered_map<std::string, ScannedFileRow> index;
            for (auto &r : ScannedFilesOps::listAll(*db_))
            {
                index.emplace(r.file_path, r);
            }
            logger.information(std::string("Loaded rows into index: ") + std::to_string(index.size()));

            auto locations = filesService_->listMediaLocations();
            logger.information(std::string("Media locations to scan: ") + std::to_string(locations.size()));
            for (const auto &kv : locations)
            {
                const std::string &normRoot = kv.first;
                const std::string &root = kv.second;

                FileScannerOptions opt;
                opt.recursive = cfg_->getPropertyValue<bool>("filesservice.scan.recursive", true);
                opt.followSymlinks = cfg_->getPropertyValue<bool>("filesservice.scan.followSymlinks", false);
                opt.includeHidden = cfg_->getPropertyValue<bool>("filesservice.scan.includeHidden", true);

                logger.information(std::string("Scanning root: ") + root +
                                   ", recursive=" + (opt.recursive ? "true" : "false") +
                                   ", followSymlinks=" + (opt.followSymlinks ? "true" : "false") +
                                   ", includeHidden=" + (opt.includeHidden ? "true" : "false"));
                scan(root, opt, [this, root, &index](const FileRecord &rec)
                     { onFileEmitted(root, rec, index); });
                logger.information(std::string("Completed scanning root: ") + root);
            }

            logger.information("FilesManager run completed");
        }
        catch (const std::exception &e)
        {
            logger.error(std::string("FilesManager run failed: ") + e.what());
        }

        running_ = false;
    }
}
