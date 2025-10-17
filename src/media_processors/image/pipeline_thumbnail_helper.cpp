#include "media_processors/image/pipeline_thumbnail_helper.hpp"
#include "database/database_manager.hpp"
#include "database/thumbnail_cache_ops.hpp"
#include "filesmanager/disk_cache.hpp"
#include "config/unified_observable_config.hpp"
#include "utils/thumbnail_generator.hpp"
#include <Poco/Logger.h>
#include <filesystem>
#include <chrono>
#include <sstream>
#include <iomanip>

namespace MediaDedup
{
    bool PipelineThumbnailHelper::generateThumbnail(const std::string &file_path,
                                                    DatabaseManager &db,
                                                    DiskCache &thumbnail_cache,
                                                    UnifiedObservableConfigManager &config_manager)
    {
        Poco::Logger &logger = Poco::Logger::get("PipelineThumbnailHelper");

        try
        {
            // Check if pipeline thumbnail generation is enabled
            bool enabled = config_manager.getPropertyValue<bool>("thumbnail.pipeline.enabled", true);
            if (!enabled)
            {
                logger.debug("Pipeline thumbnail generation disabled, skipping: %s", file_path);
                return true; // Not an error, just disabled
            }

            // Get configuration
            int size = config_manager.getPropertyValue<int>("thumbnail.pipeline.size", 256);
            int quality = config_manager.getPropertyValue<int>("thumbnail.jpeg.quality", 85);
            bool only_if_missing = config_manager.getPropertyValue<bool>("thumbnail.pipeline.onlyIfMissing", true);

            // Validate size
            if (!ThumbnailGenerator::isValidSize(size))
            {
                logger.warning("Invalid pipeline thumbnail size %d, using closest valid size", size);
                size = ThumbnailGenerator::getClosestValidSize(size);
            }

            // Check if source file exists
            if (!std::filesystem::exists(file_path))
            {
                logger.warning("Source file not found for thumbnail generation: %s", file_path);
                return false;
            }

            // Check if thumbnail already exists (if configured to skip existing)
            if (only_if_missing && thumbnailExists(file_path, size, db))
            {
                logger.debug("Thumbnail already exists, skipping generation: %s (size: %d)", file_path, size);
                return true;
            }

            // Generate unique filename for thumbnail
            std::stringstream ss;
            ss << std::hash<std::string>{}(file_path) << "_" << size << ".jpg";
            std::string cache_filename = ss.str();

            // Use thumbnail cache to get full path
            std::string temp_cached_path;
            std::filesystem::path cache_dir = std::filesystem::path(thumbnail_cache.getCacheLocation());
            temp_cached_path = (cache_dir / cache_filename).string();

            // Get source file modified time
            int64_t source_mtime = getFileModifiedTime(file_path);
            if (source_mtime == 0)
            {
                logger.warning("Failed to get modified time for: %s", file_path);
                return false;
            }

            // Generate the thumbnail using ImageMagick
            logger.debug("Generating pipeline thumbnail: %s -> %s (size: %d, quality: %d)",
                         file_path, temp_cached_path, size, quality);

            int timeout_ms = config_manager.getPropertyValue<int>("thumbnail.generation.timeoutMs", 5000);
            bool thumbnail_success = ThumbnailGenerator::generate(file_path, temp_cached_path, size, quality, timeout_ms);

            if (!thumbnail_success)
            {
                logger.warning("Failed to generate thumbnail for: %s", file_path);
                return false;
            }

            // Verify the file was created
            if (!std::filesystem::exists(temp_cached_path))
            {
                logger.error("Thumbnail file was not created: %s", temp_cached_path);
                return false;
            }

            // Get thumbnail file size
            int64_t thumb_size_bytes = std::filesystem::file_size(temp_cached_path);
            int64_t current_time = getCurrentTimestamp();

            // Store metadata in database
            ThumbnailCacheRecord record;
            record.source_path = file_path;
            record.cached_path = temp_cached_path;
            record.thumbnail_size = size;
            record.file_size_bytes = thumb_size_bytes;
            record.source_modified_at = source_mtime;
            record.created_at = current_time;
            record.last_accessed_at = current_time;

            bool db_success = ThumbnailCacheOps::upsertThumbnail(db, record);

            if (!db_success)
            {
                logger.warning("Failed to store thumbnail metadata in database for: %s", file_path);
                // Don't remove from cache, thumbnail is still valid
                return false;
            }

            logger.information("Successfully generated pipeline thumbnail: %s (size: %d, %zu bytes)",
                               file_path, size, thumb_size_bytes);
            return true;
        }
        catch (const std::exception &e)
        {
            logger.error("Exception in pipeline thumbnail generation for %s: %s", file_path, e.what());
            return false;
        }
        catch (...)
        {
            logger.error("Unknown exception in pipeline thumbnail generation for: %s", file_path);
            return false;
        }
    }

    bool PipelineThumbnailHelper::thumbnailExists(const std::string &file_path,
                                                  int size,
                                                  DatabaseManager &db)
    {
        try
        {
            auto cached = ThumbnailCacheOps::getThumbnail(db, file_path, size);
            if (!cached.has_value())
            {
                return false;
            }

            // Check if source file has been modified
            int64_t source_mtime = getFileModifiedTime(file_path);
            if (source_mtime == 0 || cached->source_modified_at != source_mtime)
            {
                return false;
            }

            // Check if cached file still exists
            if (!std::filesystem::exists(cached->cached_path))
            {
                return false;
            }

            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    int64_t PipelineThumbnailHelper::getFileModifiedTime(const std::string &file_path)
    {
        try
        {
            auto ftime = std::filesystem::last_write_time(file_path);
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
            return std::chrono::system_clock::to_time_t(sctp);
        }
        catch (...)
        {
            return 0;
        }
    }

    int64_t PipelineThumbnailHelper::getCurrentTimestamp()
    {
        return std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

} // namespace MediaDedup
