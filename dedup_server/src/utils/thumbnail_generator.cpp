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
#include <libraw/libraw.h>
#include "media_processors/image/backends/raw_file_detector.hpp"

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
            bool raw_thumb_loaded = false;

            if (RawFileDetector::IsRawFile(source_path))
            {
                LibRaw raw_processor;
                if (raw_processor.open_file(source_path.c_str()) == LIBRAW_SUCCESS)
                {
                    if (raw_processor.unpack_thumb() == LIBRAW_SUCCESS)
                    {
                        int err = 0;
                        libraw_processed_image_t *thumb = raw_processor.dcraw_make_mem_thumb(&err);
                        if (thumb)
                        {
                            if (thumb->type == LIBRAW_IMAGE_JPEG)
                            {
                                try {
                                    Magick::Blob thumb_blob(thumb->data, thumb->data_size);
                                    image.read(thumb_blob);
                                    raw_thumb_loaded = true;
                                    logger.debug("Successfully extracted JPEG thumbnail from RAW file: %s", source_path);
                                } catch (...) {
                                    logger.warning("Failed to parse extracted JPEG thumbnail for: %s", source_path);
                                }
                            }
                            LibRaw::dcraw_clear_mem(thumb);
                        }
                    }
                    raw_processor.recycle();
                }
            }

            try
            {
                if (!raw_thumb_loaded)
                {
                    if (size > 0)
                    {
                        image.size(Magick::Geometry(size, size)); // Hint for JPEG/PNG decode-time scaling
                    }
                    image.ping(source_path); // Fast metadata-only read first to detect corruption
                    image.read(source_path); // Now read full image data
                }
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

            image.autoOrient();

            size_t orig_w = image.columns();
            size_t orig_h = image.rows();

            if (orig_w == 0 || orig_h == 0)
            {
                logger.warning("Invalid image dimensions: %zux%zu for %s", orig_w, orig_h, source_path);
                return false;
            }

            size_t new_w = orig_w, new_h = orig_h;
            if (size > 0)
            {
                Magick::Geometry geom(size, size);
                image.resize(geom);
            }

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
            bool raw_thumb_loaded = false;

            if (RawFileDetector::IsRawFile(source_path))
            {
                LibRaw raw_processor;
                if (raw_processor.open_file(source_path.c_str()) == LIBRAW_SUCCESS)
                {
                    if (raw_processor.unpack_thumb() == LIBRAW_SUCCESS)
                    {
                        int err = 0;
                        libraw_processed_image_t *thumb = raw_processor.dcraw_make_mem_thumb(&err);
                        if (thumb)
                        {
                            if (thumb->type == LIBRAW_IMAGE_JPEG)
                            {
                                try {
                                    Magick::Blob thumb_blob(thumb->data, thumb->data_size);
                                    image.read(thumb_blob);
                                    raw_thumb_loaded = true;
                                    logger.debug("Successfully extracted JPEG thumbnail from RAW file: %s", source_path);
                                } catch (...) {
                                    logger.warning("Failed to parse extracted JPEG thumbnail for: %s", source_path);
                                }
                            }
                            LibRaw::dcraw_clear_mem(thumb);
                        }
                    }
                    raw_processor.recycle();
                }
            }

            try
            {
                if (!raw_thumb_loaded)
                {
                    if (size > 0)
                    {
                        image.size(Magick::Geometry(size, size)); // Hint for JPEG/PNG decode-time scaling
                    }
                    image.ping(source_path); // Fast metadata-only read first to detect corruption
                    image.read(source_path); // Now read full image data
                }
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

            // Apply EXIF rotation to ensure correct orientation
            image.autoOrient();

            // Convert to sRGB to ensure proper colors (RAW files often default to linear RGB or CMYK)
            if (image.colorSpace() != Magick::sRGBColorspace)
            {
                image.colorSpace(Magick::sRGBColorspace);
            }

            size_t orig_w = image.columns();
            size_t orig_h = image.rows();

            if (orig_w == 0 || orig_h == 0)
            {
                logger.warning("Invalid image dimensions: %zux%zu for %s", orig_w, orig_h, source_path);
                return false;
            }

            if (size > 0)
            {
                Magick::Geometry geom(size, size);
                image.resize(geom);
            }

            // Set JPEG quality (0-100)
            image.quality(quality);

            // Set output format to JPEG explicitly
            image.magick("JPEG");
            // Set depth to 8-bit for web compatibility
            image.depth(8);

            // Write thumbnail to blob
            image.write(&blob);

            if (blob.length() == 0)
            {
                logger.error("Thumbnail generation produced empty blob for: %s", source_path);
                return false;
            }

            logger.debug("Generated thumbnail to blob: %s (%zux%zu) -> blob (%zux%zu, %zu bytes)",
                         source_path, orig_w, orig_h, image.columns(), image.rows(), blob.length());

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
        if (requested_size == 0)
            return 0;
            
        if (requested_size < 0)
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
