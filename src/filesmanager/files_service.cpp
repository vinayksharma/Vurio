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
#include <chrono>
#include <thread>
#include <nlohmann/json.hpp>

namespace MediaDedup
{

    static std::string normalizePathForKey(const std::string &path)
    {
        std::string p = path;
        std::replace(p.begin(), p.end(), '\\', '/');
        std::transform(p.begin(), p.end(), p.begin(), [](unsigned char c)
                       { return static_cast<char>(std::tolower(c)); });
        // Remove trailing slashes for consistency (except for root paths like "/")
        while (p.length() > 1 && (p.back() == '/' || p.back() == '\\'))
        {
            p.pop_back();
        }
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
        // Try with the exact path first, then try with/without trailing slash for compatibility
        std::string old_key = makeMediaLocationKey(old_path);
        logger_.information("changeMediaLocationPath: old_key=%s", old_key);
        std::string old_location_value;
        bool key_exists = UserSettingsOps::get(db_manager_, old_key, old_location_value);

        // If not found, try with trailing slash added/removed (for backward compatibility)
        if (!key_exists)
        {
            std::string alt_path = old_path;
            if (!alt_path.empty() && alt_path.back() != '/' && alt_path.back() != '\\')
            {
                alt_path += '/';
            }
            else if (!alt_path.empty() && (alt_path.back() == '/' || alt_path.back() == '\\'))
            {
                alt_path.pop_back();
            }
            if (alt_path != old_path)
            {
                std::string alt_key = makeMediaLocationKey(alt_path);
                logger_.information("changeMediaLocationPath: Trying alternate path key: %s (for path: %s)", alt_key, alt_path);
                key_exists = UserSettingsOps::get(db_manager_, alt_key, old_location_value);
                if (key_exists)
                {
                    old_key = alt_key;
                    logger_.information("changeMediaLocationPath: Found registration with alternate path format: %s", alt_path);
                }
            }
        }

        if (!key_exists)
        {
            // Key doesn't exist in user_settings - check if files exist with this location_key
            // This handles the case where a location was deregistered but files still reference it
            auto files_with_key = ScannedFilesOps::getFilesByLocationKey(db_manager_, old_key);
            if (!files_with_key.empty())
            {
                // Files exist with this location_key but it's not registered
                // We can proceed by creating the registration entry
                logger_.warning("changeMediaLocationPath: Location key %s not in user_settings but %d files reference it. Creating registration entry.",
                                old_key, static_cast<int>(files_with_key.size()));

                // Create the registration entry so the rest of the function works
                if (!UserSettingsOps::upsert(db_manager_, old_key, old_path))
                {
                    result.error_message = "Failed to create registration entry for location key: " + old_key;
                    logger_.error("changeMediaLocationPath: %s", result.error_message);
                    return result;
                }

                old_location_value = old_path;
                logger_.information("changeMediaLocationPath: Created registration entry for location key: %s, path: %s", old_key, old_path);
            }
            else
            {
                // No files exist either - provide helpful diagnostics
                auto all_settings = UserSettingsOps::list(db_manager_);
                std::string registered_paths;
                std::string registered_keys;
                for (const auto &kv : all_settings)
                {
                    if (kv.first.rfind("mediaLocation:", 0) == 0)
                    {
                        if (!registered_paths.empty())
                        {
                            registered_paths += ", ";
                            registered_keys += ", ";
                        }
                        registered_paths += "\"" + kv.second + "\"";
                        registered_keys += kv.first;
                    }
                }

                result.error_message = "Old path is not registered and no files exist with this location key. Old path: \"" + old_path +
                                       "\", Expected key: " + old_key;
                if (!registered_paths.empty())
                {
                    result.error_message += ". Currently registered locations (paths): " + registered_paths;
                    result.error_message += ". Registered keys: " + registered_keys;
                    result.error_message += ". Note: Path matching is case-insensitive, normalizes separators, and ignores trailing slashes.";
                }
                logger_.error("changeMediaLocationPath: %s", result.error_message);
                return result;
            }
        }

        if (old_location_value.empty())
        {
            // Key exists but value is empty - data integrity issue
            result.error_message = "Old path key exists but has empty value (data integrity issue). Key: " +
                                   old_key + ", Old path: \"" + old_path +
                                   "\". Please re-register this location first.";
            logger_.error("changeMediaLocationPath: %s", result.error_message);
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

        // Update all files in batches FIRST, before changing registration
        // This ensures registration only changes if file updates succeed
        logger_.information("changeMediaLocationPath: Starting batch update of %d files", result.total_files);
        int total_updated = 0;
        int total_failed = 0;
        const int batch_size = 1000; // Process 1000 files at a time
        int offset = 0;

        // Track updates per table
        int image_artifacts_updated = 0;
        int image_artifacts_failed = 0;
        int processing_errors_updated = 0;
        int processing_errors_failed = 0;
        int duplicate_groups_updated = 0;
        int duplicate_groups_failed = 0;
        int duplicate_members_updated = 0;
        int duplicate_members_failed = 0;
        int thumbnail_cache_updated = 0;
        int thumbnail_cache_failed = 0;

        // Track failure reasons for diagnostics
        int failed_empty_relative_path = 0;
        int failed_path_conflict = 0;
        int failed_update_error = 0;

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
                    // Try to reconstruct relative_path from file_path
                    try
                    {
                        std::filesystem::path old_fs_path(old_path);
                        std::filesystem::path file_fs_path(file.file_path);

                        // Normalize paths for comparison
                        std::string old_path_normalized = old_fs_path.lexically_normal().string();
                        std::string file_path_normalized = file_fs_path.lexically_normal().string();

                        // Check if file_path starts with old_path
                        if (file_path_normalized.find(old_path_normalized) == 0)
                        {
                            // Extract the relative portion
                            std::string relative = file_path_normalized.substr(old_path_normalized.length());
                            // Remove leading slash
                            if (!relative.empty() && (relative[0] == '/' || relative[0] == '\\'))
                            {
                                relative = relative.substr(1);
                            }
                            new_relative_path = relative;
                        }
                        else
                        {
                            // Try std::filesystem::relative as fallback
                            new_relative_path = std::filesystem::relative(file_fs_path, old_fs_path).string();
                        }
                    }
                    catch (const std::exception &e)
                    {
                        logger_.warning("Failed to reconstruct relative path for: %s (error: %s)", file.file_path, e.what());
                        // Try to extract from file_path directly as last resort
                        std::string old_path_normalized = old_path;
                        std::replace(old_path_normalized.begin(), old_path_normalized.end(), '\\', '/');
                        std::string file_path_normalized = file.file_path;
                        std::replace(file_path_normalized.begin(), file_path_normalized.end(), '\\', '/');

                        if (file_path_normalized.find(old_path_normalized) == 0)
                        {
                            new_relative_path = file_path_normalized.substr(old_path_normalized.length());
                            if (!new_relative_path.empty() && (new_relative_path[0] == '/' || new_relative_path[0] == '\\'))
                            {
                                new_relative_path = new_relative_path.substr(1);
                            }
                        }
                    }
                    catch (...)
                    {
                        logger_.warning("Failed to reconstruct relative path for: %s (unknown error)", file.file_path);
                        // Try to extract from file_path directly as last resort
                        std::string old_path_normalized = old_path;
                        std::replace(old_path_normalized.begin(), old_path_normalized.end(), '\\', '/');
                        std::string file_path_normalized = file.file_path;
                        std::replace(file_path_normalized.begin(), file_path_normalized.end(), '\\', '/');

                        if (file_path_normalized.find(old_path_normalized) == 0)
                        {
                            new_relative_path = file_path_normalized.substr(old_path_normalized.length());
                            if (!new_relative_path.empty() && (new_relative_path[0] == '/' || new_relative_path[0] == '\\'))
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
                    failed_empty_relative_path++;
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

                // Check if new path already exists (UNIQUE constraint)
                auto existing_file = ScannedFilesOps::getByPath(db_manager_, new_file_path);
                if (existing_file.has_value() && existing_file->id != file.id)
                {
                    // New path already exists for a different file - this is a duplicate scenario
                    // We need to handle this: either skip, delete the old entry, or merge
                    logger_.warning("New path already exists for different file: %s (existing id=%d, current id=%d). "
                                    "This indicates duplicate files. Skipping update.",
                                    new_file_path, existing_file->id, file.id);
                    total_failed++;
                    failed_path_conflict++;
                    continue;
                }

                // Update scanned_files
                logger_.debug("Updating file id=%d: %s -> %s", file.id, file.file_path, new_file_path);
                bool updated = ScannedFilesOps::updateFilePath(db_manager_, file.id, new_file_path,
                                                               new_relative_path, new_key);
                if (!updated)
                {
                    total_failed++;
                    failed_update_error++;
                    logger_.warning("Failed to update file path (id=%d): %s -> %s", file.id, file.file_path, new_file_path);
                    continue;
                }
                total_updated++;

                // Update other tables and track results
                int rows_updated = ScannedFilesOps::updateFilePathInImageArtifacts(db_manager_, file.file_path, new_file_path);
                if (rows_updated > 0)
                {
                    image_artifacts_updated += rows_updated;
                }
                else
                {
                    image_artifacts_failed++;
                }

                rows_updated = ScannedFilesOps::updateFilePathInProcessingErrors(db_manager_, file.file_path, new_file_path);
                if (rows_updated > 0)
                {
                    processing_errors_updated += rows_updated;
                }
                else
                {
                    processing_errors_failed++;
                }

                rows_updated = ScannedFilesOps::updateFilePathInDuplicateGroups(db_manager_, file.file_path, new_file_path);
                if (rows_updated > 0)
                {
                    duplicate_groups_updated += rows_updated;
                }
                else
                {
                    duplicate_groups_failed++;
                }

                rows_updated = ScannedFilesOps::updateFilePathInDuplicateMembers(db_manager_, file.file_path, new_file_path);
                if (rows_updated > 0)
                {
                    duplicate_members_updated += rows_updated;
                }
                else
                {
                    duplicate_members_failed++;
                }

                rows_updated = ScannedFilesOps::updateFilePathInThumbnailCache(db_manager_, file.file_path, new_file_path);
                if (rows_updated > 0)
                {
                    thumbnail_cache_updated += rows_updated;
                }
                else
                {
                    thumbnail_cache_failed++;
                }
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

        // Populate update_details with per-table statistics
        result.update_details["scanned_files"] = std::make_pair(total_updated, total_failed);
        result.update_details["image_artifacts"] = std::make_pair(image_artifacts_updated, image_artifacts_failed);
        result.update_details["processing_errors"] = std::make_pair(processing_errors_updated, processing_errors_failed);
        result.update_details["duplicate_groups"] = std::make_pair(duplicate_groups_updated, duplicate_groups_failed);
        result.update_details["duplicate_members"] = std::make_pair(duplicate_members_updated, duplicate_members_failed);
        result.update_details["thumbnail_cache"] = std::make_pair(thumbnail_cache_updated, thumbnail_cache_failed);

        // Update user_settings registration AFTER file updates complete
        // Only update registration if at least some files were successfully updated
        if (total_updated > 0)
        {
            logger_.information("changeMediaLocationPath: File updates complete. Updating registration: %d updated, %d failed",
                                total_updated, total_failed);

            // Create new registration entry
            if (!UserSettingsOps::upsert(db_manager_, new_key, new_path))
            {
                logger_.error("changeMediaLocationPath: Failed to create new location entry in user_settings after file updates");
                // Don't fail the entire operation - files are already updated
                // But mark as partial success if it wasn't already
                if (result.success)
                {
                    result.partial_success = true;
                    result.error_message = "Files updated successfully but failed to update registration";
                }
            }
            else
            {
                logger_.information("changeMediaLocationPath: Created new registration entry: %s -> %s", new_key, new_path);
            }

            // Remove old registration entry only if all files were successfully updated
            // If some files failed, keep old registration to allow retry
            if (result.success)
            {
                if (!UserSettingsOps::remove(db_manager_, old_key))
                {
                    logger_.warning("changeMediaLocationPath: Failed to remove old location entry, but operation succeeded");
                }
                else
                {
                    logger_.information("changeMediaLocationPath: Removed old registration entry: %s", old_key);
                }
            }
            else
            {
                logger_.warning("changeMediaLocationPath: Keeping old registration entry due to partial failure (%d files failed). "
                                "Old key: %s. You may need to retry the operation.",
                                total_failed, old_key);
            }
        }
        else
        {
            logger_.error("changeMediaLocationPath: No files were updated, registration not changed. Total files processed: %d, Failed: %d",
                          result.total_files, total_failed);

            if (total_failed > 0)
            {
                std::string failure_details = "Failure breakdown: ";
                if (failed_empty_relative_path > 0)
                    failure_details += std::to_string(failed_empty_relative_path) + " empty relative_path, ";
                if (failed_path_conflict > 0)
                    failure_details += std::to_string(failed_path_conflict) + " path conflicts, ";
                if (failed_update_error > 0)
                    failure_details += std::to_string(failed_update_error) + " update errors, ";

                logger_.error("changeMediaLocationPath: %s", failure_details);

                result.error_message = "No files were successfully updated. " + std::to_string(total_failed) +
                                       " files failed. " + failure_details +
                                       "Check logs for detailed error messages.";
            }
            else
            {
                result.error_message = "No files were successfully updated. No files were processed (total_files=" +
                                       std::to_string(result.total_files) + ")";
            }
        }

        if (result.success)
        {
            logger_.information("Successfully changed path from %s to %s: %d files updated, registration updated",
                                old_path, new_path, total_updated);
        }
        else
        {
            logger_.warning("Partially changed path from %s to %s: %d files updated, %d files failed",
                            old_path, new_path, total_updated, total_failed);
            if (result.error_message.empty())
            {
                result.error_message = "Some files failed to update: " + std::to_string(total_failed) + " failed";
            }
        }

        return result;
    }

    FilesService::PathRemappingVerification FilesService::verifyPathRemapping(
        const std::string &old_path,
        const std::string &new_path,
        int sample_size)
    {
        PathRemappingVerification verification;
        verification.is_successful = false;
        verification.is_in_progress = false;
        verification.old_location_key_file_count = 0;
        verification.new_location_key_file_count = 0;
        verification.files_with_old_path_prefix = 0;
        verification.files_with_new_path_prefix = 0;
        verification.sampled_files_verified = 0;
        verification.sampled_files_total = 0;
        verification.old_location_registered = false;
        verification.new_location_registered = false;

        // Generate location keys
        verification.old_location_key = makeMediaLocationKey(old_path);
        verification.new_location_key = makeMediaLocationKey(new_path);

        logger_.information("verifyPathRemapping: Verifying remapping from %s to %s", old_path, new_path);
        logger_.information("verifyPathRemapping: Old key: %s, New key: %s",
                            verification.old_location_key, verification.new_location_key);

        // Check user_settings registration
        std::string old_location_value;
        verification.old_location_registered = UserSettingsOps::get(db_manager_, verification.old_location_key, old_location_value);

        std::string new_location_value;
        verification.new_location_registered = UserSettingsOps::get(db_manager_, verification.new_location_key, new_location_value);

        // Count files with old location_key (should be 0 if successful)
        verification.old_location_key_file_count = ScannedFilesOps::countFilesByLocationKey(db_manager_, verification.old_location_key);
        logger_.information("verifyPathRemapping: Files with old location_key: %d", verification.old_location_key_file_count);

        // Count files with new location_key
        verification.new_location_key_file_count = ScannedFilesOps::countFilesByLocationKey(db_manager_, verification.new_location_key);
        logger_.information("verifyPathRemapping: Files with new location_key: %d", verification.new_location_key_file_count);

        // Count files still referencing old path prefix
        // This is a best-effort check - we'll sample files and check their paths
        auto sample_old = ScannedFilesOps::getRandomFilesByLocationKey(db_manager_, verification.old_location_key,
                                                                       std::min(100, verification.old_location_key_file_count));
        for (const auto &file : sample_old)
        {
            if (file.file_path.find(old_path) == 0)
            {
                verification.files_with_old_path_prefix++;
            }
        }

        // Sample files with new location_key and verify they exist
        if (verification.new_location_key_file_count > 0)
        {
            int verification_sample_size = std::min(sample_size, verification.new_location_key_file_count);
            auto sample_new = ScannedFilesOps::getRandomFilesByLocationKey(db_manager_, verification.new_location_key, verification_sample_size);
            verification.sampled_files_total = static_cast<int>(sample_new.size());

            for (const auto &file : sample_new)
            {
                // Check if file path starts with new_path
                if (file.file_path.find(new_path) == 0)
                {
                    verification.files_with_new_path_prefix++;

                    // Verify file actually exists at the new location
                    if (std::filesystem::exists(file.file_path))
                    {
                        verification.sampled_files_verified++;
                    }
                }
            }
        }

        // Determine if remapping was successful
        // Success criteria:
        // 1. No files with old location_key (or very few)
        // 2. Files exist with new location_key
        // 3. New location is registered
        // 4. Sampled files exist at new location
        bool has_old_files = verification.old_location_key_file_count > 0;
        bool has_new_files = verification.new_location_key_file_count > 0;
        bool new_location_registered = verification.new_location_registered;
        bool files_verified = verification.sampled_files_total == 0 ||
                              (verification.sampled_files_verified > 0 &&
                               static_cast<double>(verification.sampled_files_verified) / verification.sampled_files_total >= 0.8);

        if (!has_old_files && has_new_files && new_location_registered && files_verified)
        {
            verification.is_successful = true;
            logger_.information("verifyPathRemapping: Remapping verification PASSED");
        }
        else
        {
            verification.is_successful = false;
            std::string issues;
            if (has_old_files)
                issues += "Old location_key still has " + std::to_string(verification.old_location_key_file_count) + " files. ";
            if (!has_new_files)
                issues += "No files found with new location_key. ";
            if (!new_location_registered)
                issues += "New location not registered. ";
            if (!files_verified)
                issues += "File verification failed (" + std::to_string(verification.sampled_files_verified) + "/" +
                          std::to_string(verification.sampled_files_total) + " verified). ";
            verification.error_message = "Remapping verification failed: " + issues;
            logger_.warning("verifyPathRemapping: %s", verification.error_message);
        }

        // Check if remapping is in progress by comparing counts over time
        verification.is_in_progress = false;

        // Take initial counts
        int initial_old_count = verification.old_location_key_file_count;
        int initial_new_count = verification.new_location_key_file_count;

        // Wait a short interval (1 second) and check again
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));

        // Take second counts
        int second_old_count = ScannedFilesOps::countFilesByLocationKey(db_manager_, verification.old_location_key);
        int second_new_count = ScannedFilesOps::countFilesByLocationKey(db_manager_, verification.new_location_key);

        // If counts changed, remapping is in progress
        if (initial_old_count != second_old_count || initial_new_count != second_new_count)
        {
            verification.is_in_progress = true;
            logger_.information("verifyPathRemapping: Remapping appears to be in progress (counts changed: old %d->%d, new %d->%d)",
                                initial_old_count, second_old_count, initial_new_count, second_new_count);
        }
        else
        {
            logger_.information("verifyPathRemapping: Remapping does not appear to be in progress (counts stable)");
        }

        return verification;
    }

} // namespace MediaDedup
