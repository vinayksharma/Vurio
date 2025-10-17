#include "utils/thumbnail_generator.hpp"
#include <Poco/Logger.h>
#include <filesystem>
#include <chrono>
#include <thread>
#include <future>
#include <algorithm>
#include <Magick++.h>

namespace MediaDedup
{
    bool ThumbnailGenerator::generate(const std::string &source_path,
                                      const std::string &output_path,
                                      int size,
                                      int quality,
                                      int timeout_ms)
    {
        try
        {
            Poco::Logger &logger = Poco::Logger::get("ThumbnailGenerator");

            // Validate size
            if (!isValidSize(size))
            {
                logger.warning("Invalid thumbnail size %d requested, using closest valid size", size);
                size = getClosestValidSize(size);
            }

            // Check if source file exists
            if (!std::filesystem::exists(source_path))
            {
                logger.error("Source file not found: %s", source_path);
                return false;
            }

            // Read image using ImageMagick (supports ALL formats including RAW!)
            Magick::Image image;
            image.read(source_path);

            size_t orig_w = image.columns();
            size_t orig_h = image.rows();

            if (orig_w == 0 || orig_h == 0)
            {
                logger.warning("Invalid image dimensions: %zux%zu for %s", orig_w, orig_h, source_path);
                return false;
            }

            // Calculate new dimensions maintaining aspect ratio
            size_t new_w, new_h;
            if (orig_w > orig_h)
            {
                new_w = size;
                new_h = static_cast<size_t>((static_cast<double>(orig_h) / orig_w) * size);
            }
            else
            {
                new_h = size;
                new_w = static_cast<size_t>((static_cast<double>(orig_w) / orig_h) * size);
            }

            // Ensure minimum dimensions
            if (new_w == 0)
                new_w = 1;
            if (new_h == 0)
                new_h = 1;

            // Resize image with high-quality filter
            // Geometry syntax: "WIDTHxHEIGHT!" where ! means ignore aspect ratio
            // But we already calculated correct aspect ratio above
            std::string geometry = std::to_string(new_w) + "x" + std::to_string(new_h) + "!";
            image.resize(Magick::Geometry(geometry));

            // Set JPEG quality (0-100)
            image.quality(quality);

            // Set output format to JPEG explicitly
            image.magick("JPEG");

            // Write thumbnail
            image.write(output_path);

            logger.debug("Generated thumbnail: %s (%zux%zu) -> %s (%zux%zu)",
                         source_path, orig_w, orig_h, output_path, new_w, new_h);

            return true;
        }
        catch (const Magick::Exception &e)
        {
            Poco::Logger::get("ThumbnailGenerator").error("ImageMagick exception in generate: %s", std::string(e.what()));
            return false;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ThumbnailGenerator").error("Exception in generate: %s", std::string(e.what()));
            return false;
        }
    }

    bool ThumbnailGenerator::isValidSize(int size)
    {
        for (int valid_size : VALID_SIZES)
        {
            if (size == valid_size)
                return true;
        }
        return false;
    }

    int ThumbnailGenerator::getClosestValidSize(int requested_size)
    {
        if (requested_size <= 0)
            return DEFAULT_SIZE;

        int closest = VALID_SIZES[0];
        int min_diff = std::abs(requested_size - closest);

        for (int valid_size : VALID_SIZES)
        {
            int diff = std::abs(requested_size - valid_size);
            if (diff < min_diff)
            {
                min_diff = diff;
                closest = valid_size;
            }
        }

        return closest;
    }

} // namespace MediaDedup
