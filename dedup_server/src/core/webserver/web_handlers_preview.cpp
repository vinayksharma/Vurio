#include "core/web/web_handlers_preview.hpp"
#include "database/database_manager.hpp"
#include "database/preview_cache_ops.hpp"
#include "media_processors/image/backends/transcoding_pipeline.hpp"
#include "config/unified_observable_config.hpp"
#include "filesmanager/disk_cache.hpp"
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
#include "core/web/web_handlers_websocket.hpp"

namespace MediaDedup
{

    PreviewHandler::PreviewHandler(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                                   std::shared_ptr<DatabaseManager> database_manager,
                                   std::shared_ptr<ThreadPoolManager> tpm,
                                   std::shared_ptr<DiskCache> transcoding_cache)
        : config_manager_(std::move(config_manager)),
          database_manager_(std::move(database_manager)),
          tpm_(std::move(tpm)),
          transcoding_cache_(std::move(transcoding_cache))
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
            
            if (TranscodingPipeline::NeedsQhdPreviewCache(source_path, config_manager_))
            {
                logger.information("Preview cache miss for RAW or large file. Attempting on-demand transcoding: %s", source_path);
                
                TranscodingConfig transcode_config = TranscodingPipeline::GetConfigFromManager(config_manager_);
                if (transcode_config.enabled)
                {
                    if (tpm_)
                    {
                        auto cache_instance = transcoding_cache_;
                        auto config_copy = config_manager_;
                        auto db_copy = database_manager_;
                        std::string path_copy = source_path;
                        
                        tpm_->submit("preview_generator", [config_copy, db_copy, path_copy, transcode_config, cache_instance]() {
                            std::string temp_dir = std::filesystem::temp_directory_path().string();
                            std::string temp_path;
                            if (TranscodingPipeline::TranscodeToFile(path_copy, temp_path, transcode_config, temp_dir))
                            {
                                std::string final_path;
                                if (cache_instance && cache_instance->moveToCache(temp_path, final_path)) {
                                    // QW-8: makeRecord fills source_modified_at etc. —
                                    // a zeroed record permanently fails ingest's
                                    // freshness check and forces re-transcodes.
                                    PreviewCacheOps::upsertPreview(*db_copy,
                                                                   PreviewCacheOps::makeRecord(path_copy, final_path));
                                    
                                    Poco::JSON::Object msg;
                                    msg.set("type", "media_ready");
                                    msg.set("media_type", "preview");
                                    msg.set("file_path", path_copy);
                                    
                                    std::stringstream ss;
                                    msg.stringify(ss);
                                    WebSocketStatusHandler::broadcastMessage(ss.str());
                                } else {
                                    std::filesystem::remove(temp_path);
                                }
                            }
                        }, source_path);
                        
                        // Return a 1x1 transparent PNG to prevent browser errors and free the socket
                        response.setStatus(Poco::Net::HTTPResponse::HTTP_OK);
                        response.setContentType("image/png");
                        response.set("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
                        
                        static const unsigned char transparent_png[] = {
                            0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
                            0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
                            0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00,
                            0x0a, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x00, 0x01, 0x00, 0x00,
                            0x05, 0x00, 0x01, 0x0d, 0x0a, 0x2d, 0xb4, 0x00, 0x00, 0x00, 0x00, 0x49,
                            0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82
                        };
                        
                        response.setContentLength(sizeof(transparent_png));
                        std::ostream& out = response.send();
                        out.write(reinterpret_cast<const char*>(transparent_png), sizeof(transparent_png));
                        return;
                    }
                    else
                    {
                        std::string temp_dir = std::filesystem::temp_directory_path().string();
                        std::string temp_path;
                        
                        if (TranscodingPipeline::TranscodeToFile(source_path, temp_path, transcode_config, temp_dir))
                        {
                            std::string final_path;
                            if (transcoding_cache_ && transcoding_cache_->moveToCache(temp_path, final_path)) {
                                logger.information("Successfully generated on-demand preview at: %s", final_path);

                                // QW-8: fully populated record (freshness fields).
                                PreviewCacheOps::upsertPreview(*database_manager_,
                                                               PreviewCacheOps::makeRecord(source_path, final_path));
                                
                                streamPreviewFile(final_path, response, source_modified);
                                return;
                            } else {
                                std::filesystem::remove(temp_path);
                                logger.error("Failed to move transcoded file to cache for: %s", source_path);
                                sendErrorResponse(response, "Failed to cache preview", Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);
                                return;
                            }
                        }
                        else
                        {
                            logger.error("Failed to generate on-demand preview for: %s", source_path);
                            sendErrorResponse(response, "Failed to generate preview", Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);
                            return;
                        }
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
