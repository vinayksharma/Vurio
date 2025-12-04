#include "filesmanager/files_service.hpp"
#include "database/user_settings_ops.hpp"
#include "database/scanned_files_ops.hpp"
#include "database/sql_constants.hpp"
#include "config/unified_observable_config.hpp"
#include <Poco/DigestEngine.h>
#include <Poco/SHA1Engine.h>
#include <Poco/Path.h>
#include <algorithm>
#include <filesystem>
#include <random>
#include <set>
#include <nlohmann/json.hpp>

namespace MediaDedup
{

    static std::string normalizePathForKey(const std::string &path)
    {
        std::string p = path;
        std::replace(p.begin(), p.end(), '\\', '/');
        std::transform(p.begin(), p.end(), p.begin(), [](unsigned char c)
                       { return static_cast<char>(std::tolower(c)); });
        return p;
    }

    std::string FilesService::makeMediaLocationKey(const std::string &directory_path)
    {
        std::string norm = normalizePathForKey(directory_path);
        Poco::SHA1Engine sha1;
        sha1.update(norm);
        auto digest = sha1.digest();
        std::string hex = Poco::DigestEngine::digestToHex(digest);
        return std::string("mediaLocation:") + hex;
    }

    bool FilesService::registerMediaLocation(const std::string &directory_path)
    {
        // Validate input
        if (directory_path.empty())
        {
            logger_.warning("Cannot register empty directory path");
            return false;
        }

        const std::string key = makeMediaLocationKey(directory_path);
        
        std::string old_registered_path;
        bool was_registered = UserSettingsOps::get(db_manager_, key, old_registered_path);
        
        bool success = UserSettingsOps::upsert(db_manager_, key, directory_path);

        // Only check for automatic updates if the path exists (to avoid loading huge file lists unnecessarily)
        if (success && std::filesystem::exists(directory_path) && std::filesystem::is_directory(directory_path))
        {
            // Check if this location_key already exists with files
            logger_.information("registerMediaLocation: Checking for existing files with location_key=%s", key);
            auto existing_files = ScannedFilesOps::getFilesByLocationKey(db_manager_, key);
            
            // If files exist with this location_key, check if they need path updates
            // This handles both: 1) re-registration with different path, 2) first-time registration when files already exist
            if (!existing_files.empty())
            {
                // Determine if we need to update paths
                bool needs_update = false;
                if (was_registered && old_registered_path != directory_path)
                {
                    needs_update = true;
                    logger_.information("Path re-registered with different value: old=%s, new=%s, checking %d files for automatic path updates",
                                     old_registered_path, directory_path, static_cast<int>(existing_files.size()));
                }
                else if (!was_registered)
                {
                    // First-time registration but files already exist - check if file_paths don't match registered path
                    needs_update = true;
                    logger_.information("Registering path for existing files: path=%s, %d files found, checking if paths need updates",
                                     directory_path, static_cast<int>(existing_files.size()));
                }
                
                if (needs_update)
                {
                    int files_updated = 0;
                    int files_failed = 0;
                    int files_skipped = 0;
                    
                    for (const auto &file : existing_files)
                    {
                        // Only update files that have valid relative_path
                        if (!file.relative_path.empty())
                        {
                            // Construct new file path using relative_path
                            std::filesystem::path new_base(directory_path);
                            std::filesystem::path rel_path(file.relative_path);
                            std::string new_file_path = (new_base / rel_path).string();
                            
                            // Check if file_path already matches (no update needed)
                            if (file.file_path == new_file_path)
                            {
                                files_skipped++;
                                continue;
                            }
                            
                            // Verify file exists at new path
                            if (std::filesystem::exists(new_file_path))
                            {
                                // Reconstruct relative_path from new path
                                std::string new_relative_path = file.relative_path;
                                try
                                {
                                    std::filesystem::path new_fs_path(new_file_path);
                                    std::filesystem::path new_base_fs_path(directory_path);
                                    new_relative_path = std::filesystem::relative(new_fs_path, new_base_fs_path).string();
                                    std::replace(new_relative_path.begin(), new_relative_path.end(), '\\', '/');
                                }
                                catch (...)
                                {
                                    // Keep the original relative_path
                                }
                                
                                // Update the file path
                                if (ScannedFilesOps::updateFilePath(db_manager_, file.id, new_file_path,
                                                                   new_relative_path, key))
                                {
                                    files_updated++;
                                    
                                    // Update other tables
                                    ScannedFilesOps::updateFilePathInImageArtifacts(db_manager_, file.file_path, new_file_path);
                                    ScannedFilesOps::updateFilePathInProcessingErrors(db_manager_, file.file_path, new_file_path);
                                    ScannedFilesOps::updateFilePathInDuplicateGroups(db_manager_, file.file_path, new_file_path);
                                    ScannedFilesOps::updateFilePathInDuplicateMembers(db_manager_, file.file_path, new_file_path);
                                    ScannedFilesOps::updateFilePathInThumbnailCache(db_manager_, file.file_path, new_file_path);
                                }
                                else
                                {
                                    files_failed++;
                                }
                            }
                            else
                            {
                                files_failed++;
                                logger_.debug("File does not exist at new path, skipping: %s", new_file_path);
                            }
                        }
                        else
                        {
                            files_skipped++;
                            logger_.debug("File has no relative_path, skipping: %s", file.file_path);
                        }
                    }
                    
                    if (files_updated > 0)
                    {
                        logger_.information("Automatically updated %d file paths during registration (path=%s), %d failed, %d skipped",
                                          files_updated, directory_path, files_failed, files_skipped);
                    }
                    else if (files_failed > 0)
                    {
                        logger_.warning("No files were automatically updated during registration. %d files failed (files may not exist at new path), %d skipped",
                                      files_failed, files_skipped);
                    }
                    else if (files_skipped > 0)
                    {
                        logger_.information("All %d files already have correct paths or missing relative_path, no updates needed",
                                          files_skipped);
                    }
                }
            }
        }

        // Trigger immediate job execution if registration was successful, callback is set, and immediate triggering is enabled
        if (success && path_registered_callback_ && isImmediateJobTriggerEnabled())
        {
            logger_.debug("Path registered successfully, triggering immediate job execution: %s", directory_path);
            try
            {
                path_registered_callback_(directory_path);
            }
            catch (const std::exception &e)
            {
                logger_.error("Exception in path registered callback: %s", std::string(e.what()));
            }
            catch (...)
            {
                logger_.error("Unknown exception in path registered callback");
            }
        }

        return success;
    }

    bool FilesService::deregisterMediaLocation(const std::string &directory_path)
    {
        const std::string key = makeMediaLocationKey(directory_path);
        return UserSettingsOps::remove(db_manager_, key);
    }

    std::unordered_map<std::string, std::string> FilesService::listMediaLocations()
    {
        auto all = UserSettingsOps::list(db_manager_);
        std::unordered_map<std::string, std::string> out;
        for (const auto &kv : all)
        {
            if (kv.first.rfind("mediaLocation:", 0) == 0)
            {
                // Return map normalizedPath -> originalPath to match tests and API expectations
                std::string norm = normalizePathForKey(kv.second);
                out.emplace(norm, kv.second);
            }
        }
        return out;
    }

    void FilesService::setPathRegisteredCallback(std::function<void(const std::string &)> callback)
    {
        path_registered_callback_ = std::move(callback);
        logger_.debug("Path registered callback set");
    }

    bool FilesService::isImmediateJobTriggerEnabled() const
    {
        if (!config_manager_)
        {
            return true; // Default to enabled if no config manager
        }
        return config_manager_->getPropertyValue<bool>("files.service.immediateJobTrigger.enabled", true);
    }

    FilesService::ChangePathResult FilesService::changeMediaLocationPath(
        const std::string &old_path,
        const std::string &new_path,
        int sample_size)
    {
        logger_.information("changeMediaLocationPath called: old_path=%s, new_path=%s, sample_size=%d",
                           old_path, new_path, sample_size);

        ChangePathResult result;
        result.success = false;
        result.partial_success = false;
        result.files_verified = 0;
        result.files_verified_success = 0;
        result.total_files = 0;
        result.files_updated = 0;
        result.files_failed = 0;
        result.verification_success_rate = 0.0;

        // Validate inputs
        if (old_path.empty() || new_path.empty())
        {
            result.error_message = "Old path and new path cannot be empty";
            logger_.error("changeMediaLocationPath: %s", result.error_message);
            return result;
        }

        // Check if old path is registered
        const std::string old_key = makeMediaLocationKey(old_path);
        logger_.information("changeMediaLocationPath: old_key=%s", old_key);
        std::string old_location_value;
        if (!UserSettingsOps::get(db_manager_, old_key, old_location_value) || old_location_value.empty())
        {
            result.error_message = "Old path is not registered";
            logger_.error("changeMediaLocationPath: %s (old_key=%s, found_value='%s')", result.error_message, old_key, old_location_value);
            return result;
        }
        logger_.information("changeMediaLocationPath: old path is registered, value=%s", old_location_value);

        // Check if new path exists
        if (!std::filesystem::exists(new_path) || !std::filesystem::is_directory(new_path))
        {
            result.error_message = "New path does not exist or is not a directory";
            logger_.error("changeMediaLocationPath: %s", result.error_message);
            return result;
        }

        // Get count of files for the old location
        logger_.information("changeMediaLocationPath: Getting file count for location_key=%s", old_key);
        result.total_files = ScannedFilesOps::countFilesByLocationKey(db_manager_, old_key);
        logger_.information("changeMediaLocationPath: Found %d files for location_key=%s", result.total_files, old_key);

        if (result.total_files == 0)
        {
            result.error_message = "No files found for the old location";
            logger_.error("changeMediaLocationPath: %s", result.error_message);
            return result;
        }

        // Get random sample for verification (max 100 files, or use sample_size if smaller)
        int verification_sample_size = std::min(100, std::max(sample_size, 1));
        logger_.information("changeMediaLocationPath: Getting random sample of %d files for verification", verification_sample_size);
        auto sample = ScannedFilesOps::getRandomFilesByLocationKey(db_manager_, old_key, verification_sample_size);
        logger_.information("changeMediaLocationPath: Retrieved %d sample files", static_cast<int>(sample.size()));

        // Verify files
        logger_.information("changeMediaLocationPath: Starting verification of %d sample files", static_cast<int>(sample.size()));
        int verified_success = 0;
        int verified_count = 0;
        for (const auto &file : sample)
        {
            // Construct new file path
            std::string new_file_path;
            std::string relative;

            if (!file.relative_path.empty())
            {
                relative = file.relative_path;
            }
            else
            {
                // Reconstruct relative path from old path
                try
                {
                    std::filesystem::path old_fs_path(old_path);
                    std::filesystem::path file_fs_path(file.file_path);
                    relative = std::filesystem::relative(file_fs_path, old_fs_path).string();
                }
                catch (...)
                {
                    logger_.warning("Failed to reconstruct relative path for: %s", file.file_path);
                    continue;
                }
            }

            // Normalize path separators
            std::replace(relative.begin(), relative.end(), '\\', '/');
            if (!relative.empty() && relative[0] == '/')
            {
                relative = relative.substr(1);
            }

            // Construct new path
            std::filesystem::path new_base(new_path);
            std::filesystem::path rel_path(relative);
            new_file_path = (new_base / rel_path).string();

            // Verify file exists
            logger_.debug("Verifying file: %s", new_file_path);
            try
            {
                if (!std::filesystem::exists(new_file_path))
                {
                    logger_.debug("Verification failed: file does not exist at %s", new_file_path);
                    continue;
                }
            }
            catch (const std::exception &e)
            {
                logger_.warning("Error checking file existence for %s: %s", new_file_path, e.what());
                continue;
            }
            catch (...)
            {
                logger_.warning("Unknown error checking file existence for %s", new_file_path);
                continue;
            }

            // Verify file size matches
            try
            {
                auto new_file_size = std::filesystem::file_size(new_file_path);
                logger_.debug("File size check: %s = %llu bytes", new_file_path, static_cast<unsigned long long>(new_file_size));
                uint64_t stored_size = 0;

                // Parse file_metadata to get stored size
                try
                {
                    auto metadata = nlohmann::json::parse(file.file_metadata);
                    if (metadata.contains("sizeBytes"))
                    {
                        stored_size = metadata["sizeBytes"].get<uint64_t>();
                    }
                }
                catch (...)
                {
                    logger_.warning("Failed to parse metadata for: %s", file.file_path);
                }

                if (stored_size > 0 && static_cast<uint64_t>(new_file_size) != stored_size)
                {
                    logger_.debug("Verification failed: file size mismatch for %s (stored: %llu, actual: %llu)",
                                 new_file_path, stored_size, static_cast<unsigned long long>(new_file_size));
                    continue;
                }
            }
            catch (...)
            {
                logger_.warning("Failed to get file size for: %s", new_file_path);
                continue;
            }

            verified_success++;
            verified_count++;
            if (verified_count % 5 == 0)
            {
                logger_.information("changeMediaLocationPath: Verified %d/%d files so far (%d successful)", 
                                  verified_count, static_cast<int>(sample.size()), verified_success);
            }
        }

        logger_.information("changeMediaLocationPath: Verification complete: %d/%d successful", verified_success, static_cast<int>(sample.size()));
        result.files_verified = static_cast<int>(sample.size());
        result.files_verified_success = verified_success;
        result.verification_success_rate = result.files_verified > 0
                                               ? static_cast<double>(verified_success) / result.files_verified
                                               : 0.0;

        // Check if verification passed (80% threshold)
        if (result.verification_success_rate < 0.80)
        {
            result.error_message = "Verification failed: only " + std::to_string(verified_success) +
                                   " out of " + std::to_string(result.files_verified) +
                                   " files verified successfully (" +
                                   std::to_string(static_cast<int>(result.verification_success_rate * 100)) +
                                   "% < 80% threshold)";
            logger_.error("changeMediaLocationPath: %s", result.error_message);
            return result;
        }

        // Verification passed, proceed with updates
        const std::string new_key = makeMediaLocationKey(new_path);

        // Update user_settings: create new entry, delete old entry
        if (!UserSettingsOps::upsert(db_manager_, new_key, new_path))
        {
            result.error_message = "Failed to create new location entry in user_settings";
            logger_.error("changeMediaLocationPath: %s", result.error_message);
            return result;
        }

        if (!UserSettingsOps::remove(db_manager_, old_key))
        {
            logger_.warning("Failed to remove old location entry, but continuing with file updates");
        }

        // Update all files in batches
        logger_.information("changeMediaLocationPath: Starting batch update of %d files", result.total_files);
        int total_updated = 0;
        int total_failed = 0;
        const int batch_size = 1000; // Process 1000 files at a time
        int offset = 0;
        
        while (offset < result.total_files)
        {
            logger_.information("changeMediaLocationPath: Processing batch: offset=%d, batch_size=%d", offset, batch_size);
            auto batch = ScannedFilesOps::getFilesByLocationKeyBatch(db_manager_, old_key, batch_size, offset);
            
            if (batch.empty())
            {
                logger_.information("changeMediaLocationPath: No more files to process, breaking");
                break;
            }
            
            logger_.information("changeMediaLocationPath: Processing %d files in this batch", static_cast<int>(batch.size()));
            
            for (const auto &file : batch)
        {
            // Construct new file path and relative path
            std::string new_file_path;
            std::string new_relative_path;

            if (!file.relative_path.empty())
            {
                new_relative_path = file.relative_path;
            }
            else
            {
                try
                {
                    std::filesystem::path old_fs_path(old_path);
                    std::filesystem::path file_fs_path(file.file_path);
                    new_relative_path = std::filesystem::relative(file_fs_path, old_fs_path).string();
                }
                catch (...)
                {
                    logger_.warning("Failed to reconstruct relative path for: %s", file.file_path);
                    // Try to extract from file_path directly
                    if (file.file_path.length() > old_path.length())
                    {
                        new_relative_path = file.file_path.substr(old_path.length());
                        if ((!new_relative_path.empty() && new_relative_path[0] == '/') || new_relative_path[0] == '\\')
                        {
                            new_relative_path = new_relative_path.substr(1);
                        }
                    }
                }
            }

            // Normalize path separators
            std::replace(new_relative_path.begin(), new_relative_path.end(), '\\', '/');
            if (!new_relative_path.empty() && new_relative_path[0] == '/')
            {
                new_relative_path = new_relative_path.substr(1);
            }

            // Validate that we have a valid relative path
            if (new_relative_path.empty())
            {
                logger_.warning("Cannot update file: relative_path is empty for file_path=%s, old_path=%s", file.file_path, old_path);
                total_failed++;
                continue;
            }

            // Construct new path
            std::filesystem::path new_base(new_path);
            std::filesystem::path rel_path(new_relative_path);
            new_file_path = (new_base / rel_path).string();

            // Reconstruct relative_path from new path
            try
            {
                std::filesystem::path new_fs_path(new_file_path);
                std::filesystem::path new_base_fs_path(new_path);
                new_relative_path = std::filesystem::relative(new_fs_path, new_base_fs_path).string();
                std::replace(new_relative_path.begin(), new_relative_path.end(), '\\', '/');
            }
            catch (...)
            {
                // Keep the reconstructed relative_path
            }

            // Update scanned_files
            logger_.debug("Updating file id=%d: %s -> %s", file.id, file.file_path, new_file_path);
            bool updated = ScannedFilesOps::updateFilePath(db_manager_, file.id, new_file_path,
                                                           new_relative_path, new_key);
            if (!updated)
            {
                total_failed++;
                logger_.warning("Failed to update file path (id=%d): %s -> %s", file.id, file.file_path, new_file_path);
                continue;
            }
            total_updated++;
            
            // Update other tables
            ScannedFilesOps::updateFilePathInImageArtifacts(db_manager_, file.file_path, new_file_path);
            ScannedFilesOps::updateFilePathInProcessingErrors(db_manager_, file.file_path, new_file_path);
            ScannedFilesOps::updateFilePathInDuplicateGroups(db_manager_, file.file_path, new_file_path);
            ScannedFilesOps::updateFilePathInDuplicateMembers(db_manager_, file.file_path, new_file_path);
            ScannedFilesOps::updateFilePathInThumbnailCache(db_manager_, file.file_path, new_file_path);
        }
        
        // Move to next batch
        offset += batch_size;
        logger_.information("changeMediaLocationPath: Batch complete. Total updated: %d, Total failed: %d, Progress: %d/%d", 
                          total_updated, total_failed, offset, result.total_files);
        }

        result.files_updated = total_updated;
        result.files_failed = total_failed;
        result.partial_success = (total_failed > 0);
        result.success = (total_failed == 0);

        if (result.success)
        {
            logger_.information("Successfully changed path from %s to %s: %d files updated",
                               old_path, new_path, total_updated);
        }
        else
        {
            logger_.warning("Partially changed path from %s to %s: %d files updated, %d files failed",
                          old_path, new_path, total_updated, total_failed);
            result.error_message = "Some files failed to update: " + std::to_string(total_failed) + " failed";
        }

        return result;
    }

} // namespace MediaDedup
