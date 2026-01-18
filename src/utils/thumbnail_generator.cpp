#include "utils/thumbnail_generator.hpp"
#include "media_processors/image/backends/tiff_validator.hpp"
#include <Poco/Logger.h>
#include <filesystem>
#include <chrono>
#include <thread>
#include <future>
#include <algorithm>
#include <vector>
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

            // Check if file is readable (exists() doesn't verify read permissions)
            // macOS Photos Library files may exist but not be readable due to access restrictions
            std::error_code ec;
            auto perms = std::filesystem::status(source_path, ec).permissions();
            if (ec || (perms & (std::filesystem::perms::owner_read | std::filesystem::perms::group_read | std::filesystem::perms::others_read)) == std::filesystem::perms::none)
            {
                // Note: This check may not catch all permission issues (e.g., macOS Photos Library restrictions)
                // ImageMagick will also check during actual read, but this gives us an early warning
                logger.warning("File may not be readable (permissions check): %s", source_path);
            }

            // CRITICAL: Validate TIFF files BEFORE passing to ImageMagick
            // Corrupted TIFFs cause ImageMagick assertions that kill the process
            if (TiffValidator::isTiffFile(source_path))
            {
                auto result = TiffValidator::validate(source_path);
                if (!result.is_valid)
                {
                    logger.warning("Skipping corrupted TIFF file (pre-validation failed): %s - %s",
                                   source_path, result.error_message);
                    return false; // Skip this file - would cause ImageMagick to assert/crash
                }
            }

            // Read image using ImageMagick (supports ALL formats including RAW!)
            // Use explicit exception handling to catch corruption early
            Magick::Image image;

            try
            {
                image.ping(source_path); // Fast metadata-only read first to detect corruption
                image.read(source_path); // Now read full image data
            }
            catch (const Magick::ErrorCorruptImage &e)
            {
                logger.error("Corrupted image file (skipping): %s - %s", source_path, std::string(e.what()));
                return false;
            }
            catch (const Magick::ErrorFileOpen &e)
            {
                std::string error_msg = std::string(e.what());
                // Check for permission errors and provide helpful message
                if (error_msg.find("Operation not permitted") != std::string::npos ||
                    error_msg.find("permission") != std::string::npos)
                {
                    logger.error("Cannot open image file (permission denied): %s - %s (This may be due to macOS Photos Library access restrictions)",
                                 source_path, error_msg);
                }
                else
                {
                    logger.error("Cannot open image file: %s - %s", source_path, error_msg);
                }
                return false;
            }
            catch (const Magick::Error &e)
            {
                std::string error_msg = std::string(e.what());
                // Check for permission errors in generic ImageMagick errors
                if (error_msg.find("Operation not permitted") != std::string::npos ||
                    error_msg.find("permission") != std::string::npos)
                {
                    logger.error("ImageMagick error reading file (permission denied): %s - %s (This may be due to macOS Photos Library access restrictions)",
                                 source_path, error_msg);
                }
                else
                {
                    logger.error("ImageMagick error reading file: %s - %s", source_path, error_msg);
                }
                return false;
            }

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

    bool ThumbnailGenerator::generateErrorThumbnail(const std::string &output_path,
                                                    int base_size,
                                                    int quality)
    {
        try
        {
            Poco::Logger &logger = Poco::Logger::get("ThumbnailGenerator");

            // Create a square image with light gray background
            Magick::Image image(Magick::Geometry(base_size, base_size), Magick::Color("lightgray"));
            image.fillColor(Magick::Color("lightgray"));

            // Draw a yellow warning triangle
            // Calculate triangle points (centered in image)
            size_t center_x = base_size / 2;
            size_t center_y = base_size / 2;
            size_t triangle_size = base_size * 0.6; // Triangle takes up 60% of image

            // Triangle vertices (pointing upward)
            size_t top_x = center_x;
            size_t top_y = center_y - triangle_size / 2;
            size_t left_x = center_x - triangle_size / 2;
            size_t left_y = center_y + triangle_size / 3;
            size_t right_x = center_x + triangle_size / 2;
            size_t right_y = center_y + triangle_size / 3;

            // Draw yellow triangle with black border
            std::vector<Magick::Coordinate> triangle_points;
            triangle_points.push_back(Magick::Coordinate(top_x, top_y));
            triangle_points.push_back(Magick::Coordinate(left_x, left_y));
            triangle_points.push_back(Magick::Coordinate(right_x, right_y));

            // Draw triangle with fill and stroke in one draw operation
            int border_width = std::max(2, static_cast<int>(base_size / 256));
            std::vector<Magick::Drawable> drawables;
            drawables.push_back(Magick::DrawableStrokeColor(Magick::Color("black")));
            drawables.push_back(Magick::DrawableStrokeWidth(border_width));
            drawables.push_back(Magick::DrawableFillColor(Magick::Color("yellow")));
            drawables.push_back(Magick::DrawablePolygon(triangle_points));
            image.draw(drawables);

            // Draw exclamation mark in black inside triangle
            // Use larger font size for visibility
            int font_size = static_cast<int>(base_size * 0.25); // 25% of image size
            std::string font = "Arial-Bold";
            try
            {
                image.font(font);
            }
            catch (...)
            {
                // Font might not be available, use default
                logger.debug("Font %s not available, using default", font);
            }

            // Draw exclamation mark centered in triangle
            std::vector<Magick::Drawable> text_drawables;
            text_drawables.push_back(Magick::DrawableFillColor(Magick::Color("black")));
            text_drawables.push_back(Magick::DrawablePointSize(font_size));
            text_drawables.push_back(Magick::DrawableTextAlignment(Magick::CenterAlign));
            text_drawables.push_back(Magick::DrawableText(center_x, center_y + font_size / 4, "!"));
            image.draw(text_drawables);

            // Set JPEG quality and format
            image.quality(quality);
            image.magick("JPEG");

            // Write error thumbnail
            image.write(output_path);

            logger.debug("Generated error thumbnail: %s (size: %dx%d)",
                         output_path, base_size, base_size);

            return true;
        }
        catch (const Magick::Exception &e)
        {
            Poco::Logger::get("ThumbnailGenerator").error("ImageMagick exception in generateErrorThumbnail: %s", std::string(e.what()));
            return false;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ThumbnailGenerator").error("Exception in generateErrorThumbnail: %s", std::string(e.what()));
            return false;
        }
    }

} // namespace MediaDedup
