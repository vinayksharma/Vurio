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

            // Write thumbnail to file (for backward compatibility)
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

    bool ThumbnailGenerator::generateToBlob(const std::string &source_path,
                                             int size,
                                             Magick::Blob &blob,
                                             int quality)
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
            std::string geometry = std::to_string(new_w) + "x" + std::to_string(new_h) + "!";
            image.resize(Magick::Geometry(geometry));

            // Set JPEG quality (0-100)
            image.quality(quality);

            // Set output format to JPEG explicitly
            image.magick("JPEG");

            // Write thumbnail to blob
            image.write(&blob);

            if (blob.length() == 0)
            {
                logger.error("Thumbnail generation produced empty blob for: %s", source_path);
                return false;
            }

            logger.debug("Generated thumbnail to blob: %s (%zux%zu) -> blob (%zux%zu, %zu bytes)",
                         source_path, orig_w, orig_h, new_w, new_h, blob.length());

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
