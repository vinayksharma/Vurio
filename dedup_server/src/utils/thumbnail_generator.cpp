#include "utils/thumbnail_generator.hpp"
#include "media_processors/image/backends/tiff_validator.hpp"
#include "media_processors/image/backends/raw_file_detector.hpp"
#include "media_processors/image/backends/raw_embedded_extractor.hpp"
#include "utils/subprocess_runner.hpp"
#include "utils/temp_paths.hpp"
#include "utils/tool_paths.hpp"
#include <Poco/Logger.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <Magick++.h>

namespace MediaDedup
{
    namespace
    {
        struct TempFileCleanup
        {
            std::string path;
            ~TempFileCleanup()
            {
                if (!path.empty())
                {
                    std::error_code ec;
                    std::filesystem::remove(path, ec);
                }
            }
        };

        /// Shared front half of thumbnail generation: validates the source and
        /// resolves the magick input path. RAW files route through the single
        /// consolidated decoder (RawEmbeddedExtractor); everything else gets a
        /// magic-bytes format prefix. Returns false when the source is unusable.
        bool resolveInputPath(const std::string &source_path,
                              std::string &read_path,
                              TempFileCleanup &cleanup,
                              Poco::Logger &logger)
        {
            if (!std::filesystem::exists(source_path))
            {
                logger.error("Source file not found: %s", source_path);
                return false;
            }

            // Fail fast on zero-filled/truncated copies — no decoder can read
            // them, and burning a magick attempt (and its timeout) is waste.
            if (RawFileDetector::HasZeroedHeader(source_path))
            {
                logger.error("Corrupted file (zeroed header), skipping: %s", source_path);
                return false;
            }

            // CRITICAL: Validate TIFF files BEFORE passing to ImageMagick
            if (TiffValidator::isTiffFile(source_path))
            {
                auto result = TiffValidator::validate(source_path);
                if (!result.is_valid)
                {
                    logger.warning("Skipping corrupted TIFF file (pre-validation failed): %s - %s",
                                   source_path, result.error_message);
                    return false;
                }
            }

            read_path = source_path;

            if (RawFileDetector::IsRawFile(source_path))
            {
                // Consolidated RAW path: extract the embedded JPEG (any size is
                // fine for a thumbnail). Only when the RAW file carries no
                // usable embedded image do we hand the bare RAW to magick's
                // full-demosaic delegate.
                if (auto embedded = RawEmbeddedExtractor::extract(source_path))
                {
                    cleanup.path = embedded->jpeg_path;
                    read_path = "jpeg:" + embedded->jpeg_path;
                }
                return true;
            }

            ImageFormat format = RawFileDetector::DetectFormatFromMagicBytes(source_path);
            if (format == ImageFormat::JPEG)
                read_path = "jpeg:" + source_path;
            else if (format == ImageFormat::PNG)
                read_path = "png:" + source_path;
            else if (format == ImageFormat::TIFF)
                read_path = "tiff:" + source_path;
            return true;
        }

        /// Shared back half: run magick with a hard timeout to produce the
        /// output JPEG.
        bool runMagick(const std::string &read_path,
                       const std::string &output_path,
                       int size,
                       int quality,
                       int timeout_ms,
                       const std::string &source_for_log,
                       Poco::Logger &logger)
        {
            std::vector<std::string> args =
                ThumbnailGenerator::buildMagickArgs(read_path, output_path, size, quality);

            SubprocessResult result = runProcessWithTimeout(ToolPaths::magickPath(), args, timeout_ms,
                                                            ToolPaths::magickResourceEnv());
            if (result.timed_out)
            {
                logger.error("magick timed out after %d ms for file: %s", timeout_ms, source_for_log);
                return false;
            }
            if (result.exit_code != 0)
            {
                logger.error("magick command failed with exit code %d for file: %s", result.exit_code, source_for_log);
                return false;
            }
            if (!std::filesystem::exists(output_path))
            {
                logger.error("magick succeeded but output file is missing: %s", output_path);
                return false;
            }
            return true;
        }
    }

    std::vector<std::string> ThumbnailGenerator::buildMagickArgs(const std::string &read_path,
                                                                 const std::string &output_path,
                                                                 int size,
                                                                 int quality)
    {
        std::vector<std::string> args;
        args.push_back(read_path);
        // Drop EXIF/XMP/ICC/IPTC profiles from the output: at thumbnail sizes
        // they are most of the bytes, and nothing reads metadata off a cached
        // thumbnail. Placed right after the input so every later operator works
        // on the stripped image.
        args.push_back("-strip");
        args.push_back("-depth");
        args.push_back("8");
        args.push_back("-auto-orient");
        args.push_back("-colorspace");
        args.push_back("sRGB");
        if (size > 0)
        {
            args.push_back("-resize");
            args.push_back(std::to_string(size) + "x" + std::to_string(size));
        }
        args.push_back("-quality");
        args.push_back(std::to_string(quality));
        args.push_back("jpeg:" + output_path);
        return args;
    }

    bool ThumbnailGenerator::generate(const std::string &source_path,
                                      const std::string &output_path,
                                      int size,
                                      int quality,
                                      int timeout_ms)
    {
        try
        {
            Poco::Logger &logger = Poco::Logger::get("ThumbnailGenerator");

            if (!isValidSize(size))
            {
                logger.warning("Invalid thumbnail size %d requested, using closest valid size", size);
                size = getClosestValidSize(size);
            }

            TempFileCleanup cleanup;
            std::string read_path;
            if (!resolveInputPath(source_path, read_path, cleanup, logger))
                return false;

            if (!runMagick(read_path, output_path, size, quality, timeout_ms, source_path, logger))
                return false;

            logger.debug("Generated thumbnail: %s -> %s", source_path, output_path);
            return true;
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
                                             int quality,
                                             int timeout_ms)
    {
        try
        {
            Poco::Logger &logger = Poco::Logger::get("ThumbnailGenerator");

            if (!isValidSize(size))
            {
                logger.warning("Invalid thumbnail size %d requested, using closest valid size", size);
                size = getClosestValidSize(size);
            }

            TempFileCleanup cleanup;
            std::string read_path;
            if (!resolveInputPath(source_path, read_path, cleanup, logger))
                return false;

            TempFileCleanup out_cleanup;
            out_cleanup.path = makeTempFilePath("thumb_out", ".jpg");

            if (!runMagick(read_path, out_cleanup.path, size, quality, timeout_ms, source_path, logger))
                return false;

            std::ifstream file(out_cleanup.path, std::ios::binary | std::ios::ate);
            if (!file.is_open())
            {
                logger.error("Failed to read temporary output file: %s", out_cleanup.path);
                return false;
            }

            std::streamsize out_size = file.tellg();
            file.seekg(0, std::ios::beg);

            if (out_size <= 0)
            {
                logger.error("Generated temporary file is empty: %s", out_cleanup.path);
                return false;
            }

            std::vector<std::uint8_t> data(out_size);
            if (file.read(reinterpret_cast<char *>(data.data()), out_size))
            {
                blob.update(data.data(), out_size);
                logger.debug("Generated thumbnail to blob: %s -> blob (%zu bytes)", source_path, static_cast<size_t>(out_size));
                return true;
            }

            logger.error("Failed to read data from temporary output file: %s", out_cleanup.path);
            return false;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ThumbnailGenerator").error("Exception in generateToBlob: %s", std::string(e.what()));
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
