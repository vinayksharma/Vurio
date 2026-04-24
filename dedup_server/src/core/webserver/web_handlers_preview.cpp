#include "core/web/web_handlers_preview.hpp"
#include "database/database_manager.hpp"
#include "database/preview_cache_ops.hpp"
#include "media_processors/image/backends/transcoding_pipeline.hpp"
#include "config/unified_observable_config.hpp"
#include "utils/thumbnail_generator.hpp"
#include <Magick++.h>
#include <Poco/Net/HTMLForm.h>
#include <Poco/URI.h>
#include <Poco/File.h>
#include <Poco/FileStream.h>
#include <Poco/StreamCopier.h>
#include <Poco/Logger.h>
#include <Poco/DateTimeFormatter.h>
#include <Poco/DateTimeFormat.h>
#include <Poco/Timestamp.h>
#include <Poco/Exception.h>

namespace MediaDedup
{

    PreviewHandler::PreviewHandler(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                                   std::shared_ptr<DatabaseManager> database_manager)
        : config_manager_(std::move(config_manager)),
          database_manager_(std::move(database_manager))
    {
    }

    void PreviewHandler::handleRequest(Poco::Net::HTTPServerRequest &request,
                                       Poco::Net::HTTPServerResponse &response)
    {
        Poco::Logger &logger = Poco::Logger::get("PreviewHandler");

        try
        {
            if (request.getMethod() != "GET")
            {
                sendErrorResponse(response, "Method Not Allowed", Poco::Net::HTTPResponse::HTTP_METHOD_NOT_ALLOWED);
                return;
            }

            Poco::URI uri(request.getURI());
            Poco::URI::QueryParameters params = uri.getQueryParameters();

            std::string source_path;
            std::string size_param;
            for (const auto &param : params)
            {
                if (param.first == "path")
                {
                    source_path = param.second;
                }
                else if (param.first == "size")
                {
                    size_param = param.second;
                }
            }

            if (source_path.empty())
            {
                sendErrorResponse(response, "Path cannot be empty", Poco::Net::HTTPResponse::HTTP_BAD_REQUEST);
                return;
            }

            // Check if source file exists
            Poco::File source_file(source_path);
            if (!source_file.exists())
            {
                logger.warning("Source file not found for preview: %s", source_path);
                sendErrorResponse(response, "Source file not found", Poco::Net::HTTPResponse::HTTP_NOT_FOUND);
                return;
            }

            int64_t source_modified = source_file.getLastModified().epochTime();

            if (size_param == "full")
            {
                logger.information("Generating full-size preview on demand for: %s", source_path);
                Magick::Blob blob;
                // size=0 gives native full resolution, quality 95
                if (ThumbnailGenerator::generateToBlob(source_path, 0, blob, 95))
                {
                    logger.debug("Successfully generated full size dynamically for: %s", source_path);
                    streamData(static_cast<const char*>(blob.data()), blob.length(), response, source_modified);
                    return;
                }
                else
                {
                    logger.error("Failed to generate full size dynamically for: %s", source_path);
                    sendErrorResponse(response, "Failed to generate full-size preview", Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);
                    return;
                }
            }

            // Check if we have a valid preview in the database (this cache will be QHD)
            auto preview_record = PreviewCacheOps::getPreview(*database_manager_, source_path);

            if (preview_record)
            {
                // Check if the preview is stale
                if (preview_record->source_modified_at < source_modified)
                {
                    logger.debug("Stale preview found for %s, deleting record", source_path);
                    PreviewCacheOps::deletePreview(*database_manager_, source_path);
                    // Preview is stale, so we cannot serve it
                    sendErrorResponse(response, "Preview not found (stale)", Poco::Net::HTTPResponse::HTTP_NOT_FOUND);
                    return;
                }

                // Verify the cached file actually exists
                Poco::File cached_file(preview_record->cached_path);
                if (cached_file.exists())
                {
                    // Update access time asynchronously (fire and forget)
                    Poco::Timestamp now;
                    PreviewCacheOps::updateAccessTime(*database_manager_, source_path, now.epochTime());

                    streamPreviewFile(preview_record->cached_path, response, preview_record->source_modified_at);
                    return;
                }
                else
                {
                    logger.warning("Database has preview record but file is missing: %s", preview_record->cached_path);
                    PreviewCacheOps::deletePreview(*database_manager_, source_path);
                    // Continue to fallback logic below
                }
            }

            // Fallback logic: Preview not found in cache (or was stale/missing)
            
            if (TranscodingPipeline::NeedsTranscoding(source_path))
            {
                logger.information("Preview cache miss for RAW file. Attempting on-demand transcoding: %s", source_path);
                
                TranscodingConfig transcode_config = TranscodingPipeline::GetConfigFromManager(config_manager_);
                if (transcode_config.enabled)
                {
                    std::string cache_dir = config_manager_->getPropertyValue<std::string>("cache.disk.location", "cache");
                    std::string generated_path;
                    
                    if (TranscodingPipeline::TranscodeToFile(source_path, generated_path, transcode_config, cache_dir))
                    {
                        logger.information("Successfully generated on-demand preview at: %s", generated_path);
                        
                        // Upsert the new record
                        PreviewCacheRecord new_record;
                        new_record.source_path = source_path;
                        new_record.cached_path = generated_path;
                        PreviewCacheOps::upsertPreview(*database_manager_, new_record);
                        
                        streamPreviewFile(generated_path, response, source_modified);
                        return;
                    }
                    else
                    {
                        logger.error("Failed to generate on-demand preview for: %s", source_path);
                        sendErrorResponse(response, "Failed to generate preview", Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);
                        return;
                    }
                }
                else
                {
                    logger.warning("Transcoding disabled in config, cannot generate on-demand preview for: %s", source_path);
                    sendErrorResponse(response, "Preview not found and transcoding is disabled", Poco::Net::HTTPResponse::HTTP_NOT_FOUND);
                    return;
                }
            }
            else
            {
                // File does not need transcoding (e.g., standard JPEG/PNG)
                logger.debug("Streaming original file directly: %s", source_path);
                streamPreviewFile(source_path, response, source_modified);
                return;
            }
        }
        catch (const Poco::Exception &e)
        {
            logger.error("Poco Exception handling preview request: %s - %s", e.name(), e.message());
            sendErrorResponse(response, "Internal Server Error", Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);
        }
        catch (const std::exception &e)
        {
            logger.error("Exception handling preview request: %s", std::string(e.what()));
            sendErrorResponse(response, "Internal Server Error", Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);
        }
    }

    void PreviewHandler::streamPreviewFile(const std::string &file_path,
                                           Poco::Net::HTTPServerResponse &response,
                                           int64_t source_modified_at)
    {
        try
        {
            Poco::File file(file_path);
            if (!file.exists() || !file.isFile())
            {
                sendErrorResponse(response, "File not found", Poco::Net::HTTPResponse::HTTP_NOT_FOUND);
                return;
            }

            // Set caching headers based on source modification time
            // This ensures browsers cache the preview until the source file changes
            Poco::Timestamp modified_ts = Poco::Timestamp::fromEpochTime(source_modified_at);
            std::string last_modified = Poco::DateTimeFormatter::format(modified_ts, Poco::DateTimeFormat::HTTP_FORMAT);

            response.setStatus(Poco::Net::HTTPResponse::HTTP_OK);
            response.setContentType("image/jpeg");
            response.set("Cache-Control", "public, max-age=31536000, immutable");
            response.set("Last-Modified", last_modified);
            response.setContentLength(file.getSize());

            Poco::FileInputStream fis(file_path);
            Poco::StreamCopier::copyStream(fis, response.send());
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("PreviewHandler").error("Failed to stream preview file %s: %s", file_path, std::string(e.what()));
            if (!response.sent())
            {
                sendErrorResponse(response, "Error reading file", Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);
            }
        }
    }

    void PreviewHandler::streamData(const char* data, size_t length,
                                    Poco::Net::HTTPServerResponse &response,
                                    int64_t source_modified_at)
    {
        try
        {
            Poco::Timestamp modified_ts = Poco::Timestamp::fromEpochTime(source_modified_at);
            std::string last_modified = Poco::DateTimeFormatter::format(modified_ts, Poco::DateTimeFormat::HTTP_FORMAT);

            response.setStatus(Poco::Net::HTTPResponse::HTTP_OK);
            response.setContentType("image/jpeg");
            response.set("Cache-Control", "public, max-age=31536000, immutable");
            response.set("Last-Modified", last_modified);
            response.setContentLength(length);

            std::ostream& out = response.send();
            out.write(data, length);
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("PreviewHandler").error("Failed to stream preview data: %s", std::string(e.what()));
            if (!response.sent())
            {
                sendErrorResponse(response, "Error writing data", Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);
            }
        }
    }

    void PreviewHandler::sendErrorResponse(Poco::Net::HTTPServerResponse &response,
                                           const std::string &error_message,
                                           int status_code)
    {
        if (response.sent())
            return;

        response.setStatusAndReason(static_cast<Poco::Net::HTTPResponse::HTTPStatus>(status_code), error_message);
        response.setContentType("application/json");

        std::string json = "{\"error\":\"" + error_message + "\"}";
        response.setContentLength(json.length());
        response.sendBuffer(json.data(), json.length());
    }

} // namespace MediaDedup
