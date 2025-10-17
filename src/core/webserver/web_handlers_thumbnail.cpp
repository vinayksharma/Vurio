#include "core/web/web_handlers_thumbnail.hpp"
#include "database/database_manager.hpp"
#include "database/thumbnail_cache_ops.hpp"
#include "utils/thumbnail_generator.hpp"
#include <Poco/JSON/Object.h>
#include <Poco/URI.h>
#include <Poco/File.h>
#include <Poco/FileStream.h>
#include <Poco/StreamCopier.h>
#include <Poco/DateTimeFormatter.h>
#include <Poco/DateTimeFormat.h>
#include <Poco/DateTime.h>
#include <Poco/Timestamp.h>
#include <sstream>
#include <filesystem>
#include <chrono>
#include <iomanip>

namespace MediaDedup
{
    // Static member initialization
    std::map<std::string, std::shared_ptr<std::mutex>> ThumbnailHandler::generation_locks_;
    std::mutex ThumbnailHandler::locks_mutex_;

    // Helper functions
    static int64_t getFileModifiedTime(const std::string &path)
    {
        try
        {
            auto ftime = std::filesystem::last_write_time(path);
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
            return std::chrono::system_clock::to_time_t(sctp);
        }
        catch (...)
        {
            return 0;
        }
    }

    static int64_t getCurrentTimestamp()
    {
        return std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    static std::string generateETag(const std::string &source_path, int64_t mtime, int size)
    {
        std::stringstream ss;
        ss << "\"" << std::hash<std::string>{}(source_path) << "-" << mtime << "-" << size << "\"";
        return ss.str();
    }

    // ThumbnailHandler Implementation
    ThumbnailHandler::ThumbnailHandler(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                                       std::shared_ptr<DatabaseManager> database_manager,
                                       std::shared_ptr<DiskCache> thumbnail_cache,
                                       std::shared_ptr<DiskCache> transcoding_cache)
        : config_manager_(std::move(config_manager)),
          database_manager_(std::move(database_manager)),
          thumbnail_cache_(std::move(thumbnail_cache)),
          transcoding_cache_(std::move(transcoding_cache))
    {
    }

    void ThumbnailHandler::handleRequest(Poco::Net::HTTPServerRequest &request,
                                         Poco::Net::HTTPServerResponse &response)
    {
        Poco::Logger &logger = Poco::Logger::get("ThumbnailHandler");

        if (request.getMethod() != "GET")
        {
            sendErrorResponse(response, "Method not allowed", 405);
            return;
        }

        try
        {
            // Parse query parameters
            Poco::URI uri(request.getURI());
            Poco::URI::QueryParameters params = uri.getQueryParameters();

            std::string source_path;
            int size = config_manager_->getPropertyValue<int>("thumbnail.default.size", 256);

            for (const auto &param : params)
            {
                if (param.first == "path")
                {
                    source_path = param.second;
                }
                else if (param.first == "size")
                {
                    try
                    {
                        size = std::stoi(param.second);
                    }
                    catch (...)
                    {
                        sendErrorResponse(response, "Invalid size parameter", 400);
                        return;
                    }
                }
            }

            if (source_path.empty())
            {
                sendErrorResponse(response, "Missing required parameter: path", 400);
                return;
            }

            // Validate size
            if (!ThumbnailGenerator::isValidSize(size))
            {
                sendErrorResponse(response, "Invalid size. Must be one of: 128, 256, 512, 1024", 400);
                return;
            }

            // Check if source file exists
            if (!std::filesystem::exists(source_path))
            {
                logger.warning("Source file not found: %s", source_path);
                sendErrorResponse(response, "Source file not found", 404);
                return;
            }

            // Get source file modified time
            int64_t source_mtime = getFileModifiedTime(source_path);

            // Check database for cached thumbnail
            auto cached = ThumbnailCacheOps::getThumbnail(*database_manager_, source_path, size);

            if (cached.has_value())
            {
                // Check if source file has been modified
                if (cached->source_modified_at == source_mtime)
                {
                    // Check if cached file still exists
                    if (std::filesystem::exists(cached->cached_path))
                    {
                        // Cache hit! Update access time and serve
                        ThumbnailCacheOps::updateAccessTime(*database_manager_, source_path, size, getCurrentTimestamp());
                        logger.debug("Cache hit for: %s (size: %d)", source_path, size);
                        streamThumbnailFile(cached->cached_path, response, source_mtime);
                        return;
                    }
                    else
                    {
                        logger.warning("Cached thumbnail file missing: %s", cached->cached_path);
                        // Fall through to regenerate
                    }
                }
                else
                {
                    logger.debug("Source file modified, regenerating thumbnail: %s", source_path);
                    // Fall through to regenerate
                }
            }

            // Cache miss or stale - generate thumbnail
            logger.debug("Cache miss for: %s (size: %d), generating...", source_path, size);

            std::string cached_path;
            if (!generateAndCacheThumbnail(source_path, size, cached_path))
            {
                logger.error("Failed to generate thumbnail for: %s", source_path);
                sendErrorResponse(response, "Failed to generate thumbnail", 500);
                return;
            }

            // Stream the newly generated thumbnail
            streamThumbnailFile(cached_path, response, source_mtime);
        }
        catch (const std::exception &e)
        {
            logger.error("Exception in handleRequest: %s", std::string(e.what()));
            sendErrorResponse(response, "Internal server error", 500);
        }
    }

    std::shared_ptr<std::mutex> ThumbnailHandler::getGenerationLock(const std::string &key)
    {
        std::lock_guard<std::mutex> lock(locks_mutex_);
        auto it = generation_locks_.find(key);
        if (it != generation_locks_.end())
        {
            return it->second;
        }
        auto new_lock = std::make_shared<std::mutex>();
        generation_locks_[key] = new_lock;
        return new_lock;
    }

    void ThumbnailHandler::releaseGenerationLock(const std::string &key)
    {
        std::lock_guard<std::mutex> lock(locks_mutex_);
        generation_locks_.erase(key);
    }

    bool ThumbnailHandler::generateAndCacheThumbnail(const std::string &source_path,
                                                     int size,
                                                     std::string &cached_path)
    {
        Poco::Logger &logger = Poco::Logger::get("ThumbnailHandler");

        // Get generation lock for this specific source+size combination
        std::string lock_key = source_path + "_" + std::to_string(size);
        auto gen_lock = getGenerationLock(lock_key);
        std::lock_guard<std::mutex> lock(*gen_lock);

        try
        {
            // Double-check cache after acquiring lock (another thread might have generated it)
            auto cached = ThumbnailCacheOps::getThumbnail(*database_manager_, source_path, size);
            int64_t source_mtime = getFileModifiedTime(source_path);

            if (cached.has_value() && cached->source_modified_at == source_mtime &&
                std::filesystem::exists(cached->cached_path))
            {
                cached_path = cached->cached_path;
                logger.debug("Thumbnail already generated by another thread: %s", source_path);
                releaseGenerationLock(lock_key);
                return true;
            }

            // Generate unique filename for thumbnail
            std::stringstream ss;
            ss << std::hash<std::string>{}(source_path) << "_" << size << ".jpg";
            std::string cache_filename = ss.str();

            // Use thumbnail cache to get full path
            std::string temp_cached_path;
            std::filesystem::path cache_dir = std::filesystem::path(thumbnail_cache_->getCacheLocation());
            temp_cached_path = (cache_dir / cache_filename).string();

            // Get configuration
            int quality = config_manager_->getPropertyValue<int>("thumbnail.jpeg.quality", 85);
            int timeout_ms = config_manager_->getPropertyValue<int>("thumbnail.generation.timeoutMs", 5000);

            // Generate thumbnail directly from source file
            // ImageMagick handles ALL formats including RAW (ARW, CR2, NEF, etc.) natively!
            logger.debug("Generating thumbnail: %s -> %s (size: %d, quality: %d)",
                         source_path, temp_cached_path, size, quality);

            bool thumbnail_success = ThumbnailGenerator::generate(source_path, temp_cached_path, size, quality, timeout_ms);

            if (!thumbnail_success)
            {
                logger.error("ThumbnailGenerator::generate failed for: %s", source_path);
                releaseGenerationLock(lock_key);
                return false;
            }

            // Check if file was actually created
            if (!std::filesystem::exists(temp_cached_path))
            {
                logger.error("Generated thumbnail file not found: %s", temp_cached_path);
                releaseGenerationLock(lock_key);
                return false;
            }

            // Get thumbnail file size
            int64_t file_size = std::filesystem::file_size(temp_cached_path);
            int64_t now_ts = getCurrentTimestamp();

            // Update database
            ThumbnailCacheRecord record;
            record.source_path = source_path;
            record.cached_path = temp_cached_path;
            record.thumbnail_size = size;
            record.file_size_bytes = file_size;
            record.source_modified_at = source_mtime;
            record.created_at = now_ts;
            record.last_accessed_at = now_ts;

            if (!ThumbnailCacheOps::upsertThumbnail(*database_manager_, record))
            {
                logger.warning("Failed to update database, but thumbnail was generated: %s", temp_cached_path);
                // Continue anyway - we have the thumbnail
            }

            cached_path = temp_cached_path;
            releaseGenerationLock(lock_key);
            logger.information("Successfully generated and cached thumbnail: %s (size: %d, %lld bytes)",
                               source_path, size, file_size);
            return true;
        }
        catch (const std::exception &e)
        {
            logger.error("Exception in generateAndCacheThumbnail: %s", std::string(e.what()));
            releaseGenerationLock(lock_key);
            return false;
        }
    }

    void ThumbnailHandler::streamThumbnailFile(const std::string &file_path,
                                               Poco::Net::HTTPServerResponse &response,
                                               int64_t source_modified_at)
    {
        try
        {
            Poco::File file(file_path);
            if (!file.exists())
            {
                sendErrorResponse(response, "Thumbnail file not found", 404);
                return;
            }

            // Set response headers
            response.setStatus(Poco::Net::HTTPResponse::HTTP_OK);
            response.setContentType("image/jpeg");
            response.setContentLength(file.getSize());

            // Cache headers
            response.set("Cache-Control", "public, max-age=31536000, immutable");

            // ETag
            std::string etag = generateETag(file_path, source_modified_at, 0);
            response.set("ETag", etag);

            // Last-Modified
            Poco::Timestamp ts = Poco::Timestamp::fromEpochTime(source_modified_at);
            Poco::DateTime dt(ts);
            std::string last_modified = Poco::DateTimeFormatter::format(dt, Poco::DateTimeFormat::HTTP_FORMAT);
            response.set("Last-Modified", last_modified);

            // Stream file
            std::ostream &out = response.send();
            Poco::FileInputStream fis(file_path);
            Poco::StreamCopier::copyStream(fis, out);

            Poco::Logger::get("ThumbnailHandler").debug("Streamed thumbnail: %s (%llu bytes)", file_path, static_cast<unsigned long long>(file.getSize()));
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ThumbnailHandler").error("Exception streaming thumbnail: %s", std::string(e.what()));
            sendErrorResponse(response, "Failed to stream thumbnail", 500);
        }
    }

    void ThumbnailHandler::sendErrorResponse(Poco::Net::HTTPServerResponse &response,
                                             const std::string &error_message,
                                             int status_code)
    {
        Poco::JSON::Object error_obj;
        error_obj.set("error", error_message);

        std::stringstream ss;
        error_obj.stringify(ss);

        response.setStatus(static_cast<Poco::Net::HTTPResponse::HTTPStatus>(status_code));
        response.setContentType("application/json; charset=utf-8");
        response.setContentLength(ss.str().length());
        response.send() << ss.str();
    }

    // ThumbnailCleanupHandler Implementation
    ThumbnailCleanupHandler::ThumbnailCleanupHandler(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                                                     std::shared_ptr<DatabaseManager> database_manager,
                                                     std::shared_ptr<DiskCache> thumbnail_cache)
        : config_manager_(std::move(config_manager)),
          database_manager_(std::move(database_manager)),
          thumbnail_cache_(std::move(thumbnail_cache))
    {
    }

    void ThumbnailCleanupHandler::handleRequest(Poco::Net::HTTPServerRequest &request,
                                                Poco::Net::HTTPServerResponse &response)
    {
        Poco::Logger &logger = Poco::Logger::get("ThumbnailCleanupHandler");

        if (request.getMethod() != "DELETE")
        {
            sendErrorResponse(response, "Method not allowed", 405);
            return;
        }

        try
        {
            // Parse query parameters
            Poco::URI uri(request.getURI());
            Poco::URI::QueryParameters params = uri.getQueryParameters();

            bool check_source = false;
            for (const auto &param : params)
            {
                if (param.first == "check_source")
                {
                    check_source = (param.second == "true" || param.second == "1");
                }
            }

            int removed_count = 0;
            int64_t freed_bytes = 0;

            if (check_source)
            {
                // Get all thumbnails from database and check if source files exist
                logger.information("Cleaning up thumbnails for non-existent source files...");

                // For now, return success with 0 removals
                // TODO: Implement full source file checking if needed
                logger.information("Source file checking not yet implemented");
            }

            // Return results
            Poco::JSON::Object result;
            result.set("removed_count", removed_count);
            result.set("freed_bytes", static_cast<long long>(freed_bytes));

            std::stringstream ss;
            result.stringify(ss);

            sendJsonResponse(response, ss.str());
        }
        catch (const std::exception &e)
        {
            logger.error("Exception in handleRequest: %s", std::string(e.what()));
            sendErrorResponse(response, "Internal server error", 500);
        }
    }

    void ThumbnailCleanupHandler::sendJsonResponse(Poco::Net::HTTPServerResponse &response,
                                                   const std::string &json_data,
                                                   int status_code)
    {
        response.setStatus(static_cast<Poco::Net::HTTPResponse::HTTPStatus>(status_code));
        response.setContentType("application/json; charset=utf-8");
        response.setContentLength(json_data.length());
        response.send() << json_data;
    }

    void ThumbnailCleanupHandler::sendErrorResponse(Poco::Net::HTTPServerResponse &response,
                                                    const std::string &error_message,
                                                    int status_code)
    {
        Poco::JSON::Object error_obj;
        error_obj.set("error", error_message);

        std::stringstream ss;
        error_obj.stringify(ss);

        sendJsonResponse(response, ss.str(), status_code);
    }

} // namespace MediaDedup
