#include "orchestration/files_manager.hpp"
#include "orchestration/duplicate_finder.hpp"
#include "database/processing_errors_ops.hpp"
#include "database/duplicate_groups_ops.hpp"
#include "database/user_settings_ops.hpp"
#include <Poco/Logger.h>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <thread>
#include <sstream>
#include <iomanip>

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

        // Ensure processing_errors table exists
        logger.information("Ensuring processing_errors table exists");
        if (!ProcessingErrorsOps::ensureTable(*db_))
        {
            logger.warning("Failed to ensure processing_errors table exists (continuing)");
        }
    }

    void FilesManager::triggerScanNow()
    {
        runOnce();
    }

    void FilesManager::abortCurrentScan()
    {
        abortScan_.store(true);
    }

    void FilesManager::onFileEmitted(const std::string &root,
                                     const FileRecord &rec)
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
            // Use database query instead of in-memory index lookup
            auto existing = ScannedFilesOps::getByPath(*db_, rec.fullPath);
            if (existing)
            {
                // store error info in metadata JSON (append fields) — simple approach
                json meta;
                try
                {
                    meta = json::parse(existing->file_metadata);
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
        // Store file_time_type duration directly - no conversion needed!
        meta["createdAt"] = rec.createdAt.time_since_epoch().count();
        meta["modifiedAt"] = rec.modifiedAt.time_since_epoch().count();

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
        row.is_network_file = rec.isShareMapped;                      // best-effort
        row.location_key = filesService_->makeMediaLocationKey(root); // Full user_settings.key format

        bool is_ignored = false;
        std::string ignore_reason;

        if (cfg_->getPropertyValue<bool>("media.filter.enabled", true)) {
            int min_kb = cfg_->getPropertyValue<int>("media.filter.min_file_size_kb", 10);
            if (rec.fileSizeBytes < static_cast<uint64_t>(min_kb) * 1024) {
                is_ignored = true;
                ignore_reason = "File size (" + std::to_string(rec.fileSizeBytes / 1024) + " KB) is below minimum threshold (" + std::to_string(min_kb) + " KB)";
                row.processed = 3;
                row.ignore_reason = ignore_reason;
            }
        }

        // Use database query instead of in-memory index lookup (precheck)
        auto found = ScannedFilesOps::getByPath(*db_, rec.fullPath);
        const bool exists = found.has_value();
        bool significant_change = false;

        if (exists)
        {
            // Parse existing metadata to compare only relevant fields: size, creation date, modification date
            try
            {
                json existing_meta = json::parse(found->file_metadata);
                json new_meta = json::parse(row.file_metadata);

                // Only consider it a significant change if any of these three fields changed:
                // 1. File size
                // 2. Creation date (only if both are non-zero - actual file timestamps)
                // 3. Modification date (only if both are non-zero - actual file timestamps)
                // 4. Ignored status (if a file was previously ignored and now shouldn't be, or vice-versa)
                bool size_changed = false;
                bool creation_changed = false;
                bool modification_changed = false;
                bool ignore_status_changed = (found->processed == 3) != is_ignored;

                if (existing_meta.contains("sizeBytes") && new_meta.contains("sizeBytes"))
                {
                    size_changed = (existing_meta["sizeBytes"] != new_meta["sizeBytes"]);
                }

                // Only compare creation time if both are non-zero (actual file timestamps)
                if (existing_meta.contains("createdAt") && new_meta.contains("createdAt"))
                {
                    auto existing_created = existing_meta["createdAt"].get<int64_t>();
                    auto new_created = new_meta["createdAt"].get<int64_t>();
                    if (existing_created != 0 && new_created != 0)
                    {
                        creation_changed = (existing_created != new_created);
                    }
                }

                // Only compare modification time if both are non-zero (actual file timestamps)
                if (existing_meta.contains("modifiedAt") && new_meta.contains("modifiedAt"))
                {
                    auto existing_modified = existing_meta["modifiedAt"].get<int64_t>();
                    auto new_modified = new_meta["modifiedAt"].get<int64_t>();
                    if (existing_modified != 0 && new_modified != 0)
                    {
                        modification_changed = (existing_modified != new_modified);
                    }
                }

                significant_change = size_changed || creation_changed || modification_changed || ignore_status_changed;
            }
            catch (...)
            {
                // If JSON parsing fails, fall back to string comparison but preserve processing status
                significant_change = (found->file_metadata != row.file_metadata);
            }
        }

        logger.trace("Precheck " + rec.fullPath + ": exists=" + (exists ? "yes" : "no") +
                     ", significant_change=" + (significant_change ? "yes" : "no"));

        // Only reset processing status for new files or significant changes (like file size changes)
        if (!exists || significant_change)
        {
            // Trace: Log database update for new/significantly changed files
            logger.trace("Updating database for file: " + rec.fullPath +
                         " (size: " + std::to_string(rec.fileSizeBytes) + " bytes, " +
                         (!exists ? "new" : "significantly changed - size/creation/modification date") + ")");

            if (!exists)
            {
                // New file - reset processing status to 0, or 3 if ignored.
                // DB-7: accumulated and written in one transaction per batch —
                // per-file autocommit upserts made a 160k-file scan cost 160k
                // WAL commits. The changed-file path below stays immediate
                // (rare, and ordered against the remove/re-insert sequence).
                row.processed = is_ignored ? 3 : 0;
                pending_upserts_.push_back(row);
                if (pending_upserts_.size() >= kUpsertBatchSize)
                    flushPendingUpserts();
            }
            else
            {
                // Significant change: use the "Hard Approach" to force Duplicate Finder re-evaluation
                
                // 1. Check if it's a representative of any duplicate group
                auto groups = DuplicateGroupsOps::getGroupsForFile(*db_, found->id, "EMBEDDING");
                for (const auto& group : groups) {
                    if (group.representative_file_id == found->id) {
                        if (group.member_count > 2) {
                            // Group has > 2 members: Promote next best representative so the group survives
                            std::string purity_str = cfg_->getPropertyValue<std::string>("duplicates.purity.order", "CAMERA_RAW, EXIF_CAPTURE_DATE, FILE_SIZE");
                            DuplicateFinder::promoteNextBestRepresentative(*db_, group.id, found->id, purity_str);
                        } else {
                            // Group has exactly 2 members: Do nothing.
                            // The ON DELETE CASCADE will destroy the group, causing the single remaining
                            // member to gracefully revert to an "ungrouped file" state, ready to be paired again.
                            logger.information("File %d is representative of 2-member group %d. Group will be intentionally destroyed.", found->id, group.id);
                        }
                    }
                }
                
                // 2. Completely delete the existing record (cascades to duplicate_members/groups)
                ScannedFilesOps::removeByPath(*db_, rec.fullPath);
                
                // 3. Re-insert as a brand new record (gets a new ID > last_processed_id)
                row.processed = is_ignored ? 3 : 0;
                ScannedFilesOps::upsert(*db_, row);
            }
        }
        else
        {
            // Minor metadata change or no change - preserve existing processing status
            // This prevents completed files (status=2) from being reset to 0
            // Do NOT upsert the row when unchanged to avoid any accidental state overwrites
            // Optionally update only metadata if needed in future via a dedicated method
            logger.trace("Skipping database update (no significant change) for file: " + rec.fullPath);
            return;
        }
    }

    void FilesManager::runOnce()
    {
        std::unique_lock<std::mutex> lk(runMutex_, std::try_to_lock);
        if (!lk.owns_lock() || running_)
        {
            Poco::Logger &logger = Poco::Logger::get("FilesManager");
            logger.information("Scan request rejected: scan already in progress or mutex locked, queuing rerun.");
            pendingReRun_.store(true);
            return;
        }
        running_ = true;
        abortScan_.store(false);
        Poco::Logger &logger = Poco::Logger::get("FilesManager");
        logger.information("FilesManager scan started");

        try
        {
            do {
                pendingReRun_.store(false);

            // Use database queries instead of in-memory index for better memory efficiency
            logger.information("Using database-backed file tracking (optimized for low memory usage)");

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
                    opt.shouldAbort = [this]() { return abortScan_.load(); };

                    std::string eject_path, errored_path, ignored_path;
                    // kv.first starts with "mediaLocation:", so we can just use the key suffix to match ejection path keys
                    std::string loc_key = kv.first;
                    if (loc_key.rfind("mediaLocation:", 0) == 0) {
                        loc_key = loc_key.substr(14);
                    }
                    if (MediaDedup::UserSettingsOps::get(*db_, "ejectionPath:" + loc_key, eject_path)) {
                        opt.ignoredPaths.push_back(eject_path);
                    }
                    if (MediaDedup::UserSettingsOps::get(*db_, "ejectionPathErrored:" + loc_key, errored_path)) {
                        opt.ignoredPaths.push_back(errored_path);
                    }
                    if (MediaDedup::UserSettingsOps::get(*db_, "ejectionPathIgnored:" + loc_key, ignored_path)) {
                        opt.ignoredPaths.push_back(ignored_path);
                    }

                    logger.information(std::string("Scanning root: ") + root +
                                       ", recursive=" + (opt.recursive ? "true" : "false") +
                                       ", followSymlinks=" + (opt.followSymlinks ? "true" : "false") +
                                       ", includeHidden=" + (opt.includeHidden ? "true" : "false") +
                                       ", ignoredPathsCount=" + std::to_string(opt.ignoredPaths.size()));
                    scan(root, opt, [this, root](const FileRecord &rec)
                         { onFileEmitted(root, rec); });
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
                // DB-7: write this root's remaining accumulated new files
                // (also runs after an aborted/failed root scan).
                flushPendingUpserts();
            }

            logger.information("FilesManager scan pass completed");
            } while (pendingReRun_.load() && !abortScan_.load());
        }
        catch (const std::exception &e)
        {
            logger.error("FilesManager scan failed: %s", e.what());
        }

        // Safety net: nothing accumulated may outlive the scan pass.
        flushPendingUpserts();
        running_ = false;
    }

    void FilesManager::flushPendingUpserts()
    {
        if (pending_upserts_.empty())
            return;
        const std::size_t count = pending_upserts_.size();
        int written = ScannedFilesOps::upsertBatch(*db_, pending_upserts_);
        if (written != static_cast<int>(count))
        {
            Poco::Logger::get("FilesManager").error(
                "Batched upsert wrote %d of %zu scanned rows; falling back to per-row writes", written, count);
            // All-or-nothing batch failed: write individually so one bad row
            // cannot drop the whole batch.
            if (written == 0)
            {
                for (const auto &row : pending_upserts_)
                    ScannedFilesOps::upsert(*db_, row);
            }
        }
        pending_upserts_.clear();
    }
}
