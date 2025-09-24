#include "orchestration/files_manager.hpp"
#include <Poco/Logger.h>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <algorithm>

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

        // Check if this file has a supported media extension
        if (!mediaProcessor_)
        {
            logger.warning("MediaProcessor not available, skipping media type filtering");
            return;
        }

        // Get all supported media extensions from configuration
        auto supported_extensions = mediaProcessor_->getAllSupportedMediaExtensions();

        // Check if the file extension is in the supported list
        std::string extension = rec.extension;
        std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);

        if (supported_extensions.find(extension) == supported_extensions.end())
        {
            // File extension is not supported, skip inserting into database
            logger.trace("Skipping unsupported file type: " + rec.fullPath + " (extension: " + extension + ")");
            return;
        }

        logger.trace("Processing supported media file: " + rec.fullPath + " (extension: " + extension + ")");
        if (rec.hasError())
        {
            std::string path = rec.fullPath.empty() ? std::string("<empty_path>") : rec.fullPath;
            std::string errorMsg = rec.errorMessage.empty() ? std::string("<no_error_message>") : rec.errorMessage;
            std::string errorCode = to_string(rec.error);
            std::string errnoStr = rec.platformErrno != 0 ? std::to_string(rec.platformErrno) : "0";

            // Enhanced error logging with more context
            logger.warning(std::string("Scan error on ") + path +
                           " [ErrorCode: " + errorCode +
                           ", Errno: " + errnoStr +
                           ", Message: " + errorMsg + "]");
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
        // Build metadata JSON - only store fields relevant for change detection
        json meta;
        meta["sizeBytes"] = rec.fileSizeBytes;
        meta["createdAt"] = std::chrono::duration_cast<std::chrono::seconds>(rec.createdAt.time_since_epoch()).count();
        meta["modifiedAt"] = std::chrono::duration_cast<std::chrono::seconds>(rec.modifiedAt.time_since_epoch()).count();

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
        bool significant_change = false;

        if (exists)
        {
            // Parse existing metadata to compare only relevant fields: size, creation date, modification date
            try
            {
                json existing_meta = json::parse(found->second.file_metadata);
                json new_meta = json::parse(row.file_metadata);
                
                // Only consider it a significant change if any of these three fields changed:
                // 1. File size
                // 2. Creation date  
                // 3. Modification date
                bool size_changed = false;
                bool creation_changed = false;
                bool modification_changed = false;
                
                if (existing_meta.contains("sizeBytes") && new_meta.contains("sizeBytes"))
                {
                    size_changed = (existing_meta["sizeBytes"] != new_meta["sizeBytes"]);
                }
                
                if (existing_meta.contains("createdAt") && new_meta.contains("createdAt"))
                {
                    creation_changed = (existing_meta["createdAt"] != new_meta["createdAt"]);
                }
                
                if (existing_meta.contains("modifiedAt") && new_meta.contains("modifiedAt"))
                {
                    modification_changed = (existing_meta["modifiedAt"] != new_meta["modifiedAt"]);
                }
                
                significant_change = size_changed || creation_changed || modification_changed;
            }
            catch (...)
            {
                // If JSON parsing fails, fall back to string comparison but preserve processing status
                significant_change = (found->second.file_metadata != row.file_metadata);
            }
        }

        // Only reset processing status for new files or significant changes (like file size changes)
        if (!exists || significant_change)
        {
            // Trace: Log database update for new/significantly changed files
            logger.trace("Updating database for file: " + rec.fullPath +
                         " (size: " + std::to_string(rec.fileSizeBytes) + " bytes, " +
                         (!exists ? "new" : "significantly changed - size/creation/modification date") + ")");

            if (!exists)
            {
                // New file - reset all processing status to 0
                row.processed_fast = 0;
                row.processed_balanced = 0;
                row.processed_quality = 0;
            }
            else
            {
                // Significant change (like file size change) - reset processing status to 0
                // This ensures files are re-processed if their content actually changed
                row.processed_fast = 0;
                row.processed_balanced = 0;
                row.processed_quality = 0;
            }
        }
        else
        {
            // Minor metadata change or no change - preserve existing processing status
            // This prevents completed files (status=2) from being reset to 0
            row.processed_fast = found->second.processed_fast;
            row.processed_balanced = found->second.processed_balanced;
            row.processed_quality = found->second.processed_quality;

            // Still update the metadata in case of minor changes, but preserve processing status
            logger.trace("Updating metadata for file: " + rec.fullPath + " (preserving processing status)");
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
            logger.trace("Accessing database to retrieve scanned_files table data");
            std::unordered_map<std::string, ScannedFileRow> index;
            for (auto &r : ScannedFilesOps::listAll(*db_))
            {
                index.emplace(r.file_path, r);
            }
            logger.information(std::string("Loaded rows into index: ") + std::to_string(index.size()));

            logger.trace("Accessing database to retrieve media locations for monitoring");
            auto locations = filesService_->listMediaLocations();
            logger.information(std::string("Media locations to scan: ") + std::to_string(locations.size()));

            // Trace: List all directories being monitored
            logger.trace("Directories being monitored:");
            for (const auto &kv : locations)
            {
                logger.trace("  - " + kv.first + " -> " + kv.second);
            }
            for (const auto &kv : locations)
            {
                const std::string &normRoot = kv.first;
                const std::string &root = kv.second;

                try
                {
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
                catch (const std::exception &e)
                {
                    // Use safe string concatenation to avoid format string issues
                    std::string errorMsg = std::string("Failed to scan directory '") + root +
                                           "' (normalized: '" + normRoot + "'): " +
                                           e.what() + ". Exception type: " + typeid(e).name() +
                                           ". Continuing with next directory...";
                    logger.error(errorMsg);
                    // Continue with the next directory instead of failing the entire scan
                }
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
