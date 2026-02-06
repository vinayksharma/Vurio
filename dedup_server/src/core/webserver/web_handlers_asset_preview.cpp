#include "core/web/web_handlers_asset_preview.hpp"
#include "utils/thumbnail_generator.hpp"
#include <Poco/URI.h>
#include <Poco/File.h>
#include <Poco/FileStream.h>
#include <Poco/StreamCopier.h>
#include <Poco/DateTimeFormatter.h>
#include <Poco/DateTimeFormat.h>
#include <Poco/DateTime.h>
#include <Poco/Timestamp.h>
#include <Poco/Logger.h>
#include <Magick++.h>
#include <filesystem>
#include <sstream>
#include <chrono>

namespace MediaDedup
{
    std::map<std::string, std::shared_ptr<std::mutex>> AssetPreviewHandler::generation_locks_;
    std::mutex AssetPreviewHandler::locks_mutex_;

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

    AssetPreviewHandler::AssetPreviewHandler(std::shared_ptr<UnifiedObservableConfigManager> config_manager)
        : config_manager_(std::move(config_manager))
    {
    }

    std::shared_ptr<std::mutex> AssetPreviewHandler::getGenerationLock(const std::string &key)
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

    void AssetPreviewHandler::releaseGenerationLock(const std::string &key)
    {
        std::lock_guard<std::mutex> lock(locks_mutex_);
        generation_locks_.erase(key);
    }

    void AssetPreviewHandler::handleRequest(Poco::Net::HTTPServerRequest &request,
                                            Poco::Net::HTTPServerResponse &response)
    {
        Poco::Logger &logger = Poco::Logger::get("AssetPreviewHandler");

        if (request.getMethod() != "GET")
        {
            sendJsonError(response, "Method not allowed", 405);
            return;
        }

        try
        {
            Poco::URI uri(request.getURI());
            auto params = uri.getQueryParameters();

            std::string source_path;
            int max_width = 0; // 0 => no resize (highest resolution)
            int quality = config_manager_->getPropertyValue<int>("assetPreview.jpeg.quality", 85);

            for (const auto &p : params)
            {
                if (p.first == "path")
                {
                    source_path = p.second;
                }
                else if (p.first == "maxWidth")
                {
                    try
                    {
                        max_width = std::stoi(p.second);
                    }
                    catch (...)
                    {
                        sendJsonError(response, "Invalid maxWidth", 400);
                        return;
                    }
                }
                else if (p.first == "quality")
                {
                    try
                    {
                        quality = std::stoi(p.second);
                    }
                    catch (...)
                    {
                        sendJsonError(response, "Invalid quality", 400);
                        return;
                    }
                }
            }

            if (source_path.empty())
            {
                sendJsonError(response, "Missing required parameter: path", 400);
                return;
            }

            if (!std::filesystem::exists(source_path))
            {
                sendJsonError(response, "Source file not found", 404);
                return;
            }

            std::string cached_output;
            int64_t mtime = getFileModifiedTime(source_path);

            // Key uses path+params to avoid duplicate work
            std::stringstream keyss;
            keyss << source_path << "|w=" << max_width << "|q=" << quality;
            auto lock = getGenerationLock(keyss.str());
            std::lock_guard<std::mutex> guard(*lock);

            if (!ensureCachedJpeg(source_path, max_width, quality, cached_output))
            {
                // Generate small error JPEG on failure
                try
                {
                    std::string cache_dir = config_manager_->getPropertyValue<std::string>("cache.assetPreview.location", "cache/asset_previews");
                    std::filesystem::create_directories(cache_dir);
                    std::string err_path = (std::filesystem::path(cache_dir) / "error.jpg").string();

                    Magick::Image err_image(Magick::Geometry(320, 200), Magick::Color("#333333"));
                    err_image.fillColor("white");
                    err_image.fontPointsize(16);
                    err_image.annotate("Error generating preview", Magick::Geometry(10, 100));
                    err_image.magick("JPEG");
                    err_image.quality(70);
                    err_image.write(err_path);

                    streamJpegFile(err_path, response, mtime);
                }
                catch (...)
                {
                    sendJsonError(response, "Failed to generate preview", 500);
                }
                releaseGenerationLock(keyss.str());
                return;
            }

            streamJpegFile(cached_output, response, mtime);
            releaseGenerationLock(keyss.str());
        }
        catch (const std::exception &e)
        {
            logger.error("Exception in handleRequest: %s", std::string(e.what()));
            sendJsonError(response, "Internal server error", 500);
        }
    }

    bool AssetPreviewHandler::ensureCachedJpeg(const std::string &source_path,
                                               int max_width,
                                               int quality,
                                               std::string &cached_output_path)
    {
        Poco::Logger &logger = Poco::Logger::get("AssetPreviewHandler");

        // Compute cache file name
        std::stringstream name;
        name << std::hash<std::string>{}(source_path) << "_";
        if (max_width > 0)
            name << "w" << max_width << "_";
        name << "q" << quality << ".jpg";

        std::string cache_dir = config_manager_->getPropertyValue<std::string>("cache.assetPreview.location", "cache/asset_previews");
        std::filesystem::create_directories(cache_dir);
        std::filesystem::path out_path = std::filesystem::path(cache_dir) / name.str();

        // If file exists and source not modified newer than target, reuse
        try
        {
            if (std::filesystem::exists(out_path))
            {
                auto src_time = std::filesystem::last_write_time(source_path);
                auto out_time = std::filesystem::last_write_time(out_path);
                if (out_time >= src_time)
                {
                    cached_output_path = out_path.string();
                    return true;
                }
            }
        }
        catch (...)
        {
            // Fall through and regenerate
        }

        // Generate
        try
        {
            // If no resizing requested and source is JPEG, copy into cache
            bool is_jpeg = false;
            try
            {
                Magick::Image probe;
                probe.ping(source_path);
                std::string fmt = probe.magick();
                for (char &c : fmt)
                    c = static_cast<char>(::tolower(c));
                is_jpeg = (fmt == "jpeg" || fmt == "jpg");
            }
            catch (...)
            {
                // Will try full read below
            }

            if (max_width <= 0 && is_jpeg)
            {
                std::filesystem::copy_file(source_path, out_path, std::filesystem::copy_options::overwrite_existing);
                cached_output_path = out_path.string();
                return true;
            }

            if (max_width > 0)
            {
                int timeout_ms = config_manager_->getPropertyValue<int>("assetPreview.generation.timeoutMs", 8000);
                // Use thumbnail generator for resizing; it snaps to nearest valid size
                int size = ThumbnailGenerator::getClosestValidSize(max_width);
                bool ok = ThumbnailGenerator::generate(source_path, out_path.string(), size, quality, timeout_ms);
                if (!ok)
                {
                    logger.error("ThumbnailGenerator failed for: %s", source_path);
                    return false;
                }
            }
            else
            {
                // No resize: transcode to JPEG with quality only
                Magick::Image img;
                img.read(source_path);
                img.quality(quality);
                img.magick("JPEG");
                img.write(out_path.string());
            }

            cached_output_path = out_path.string();
            return true;
        }
        catch (const std::exception &e)
        {
            logger.error("Failed to generate asset preview: %s", std::string(e.what()));
            return false;
        }
    }

    void AssetPreviewHandler::streamJpegFile(const std::string &file_path,
                                             Poco::Net::HTTPServerResponse &response,
                                             int64_t source_modified_at)
    {
        Poco::File file(file_path);
        if (!file.exists())
        {
            sendJsonError(response, "Cached file not found", 404);
            return;
        }

        response.setStatus(Poco::Net::HTTPResponse::HTTP_OK);
        response.setContentType("image/jpeg");
        response.setContentLength(file.getSize());
        response.set("Cache-Control", "public, max-age=31536000, immutable");

        Poco::Timestamp ts = Poco::Timestamp::fromEpochTime(source_modified_at);
        Poco::DateTime dt(ts);
        std::string last_modified = Poco::DateTimeFormatter::format(dt, Poco::DateTimeFormat::HTTP_FORMAT);
        response.set("Last-Modified", last_modified);

        std::ostream &out = response.send();
        Poco::FileInputStream fis(file_path);
        Poco::StreamCopier::copyStream(fis, out);
    }

    void AssetPreviewHandler::sendJsonError(Poco::Net::HTTPServerResponse &response,
                                            const std::string &message,
                                            int status_code)
    {
        std::string json = std::string("{\"error\":\"") + message + "\"}";
        response.setStatus(static_cast<Poco::Net::HTTPResponse::HTTPStatus>(status_code));
        response.setContentType("application/json; charset=utf-8");
        response.setContentLength(json.size());
        response.send() << json;
    }
}
