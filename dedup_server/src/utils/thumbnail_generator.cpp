#include "utils/thumbnail_generator.hpp"
#include "media_processors/image/backends/tiff_validator.hpp"
#include "media_processors/image/backends/raw_file_detector.hpp"
#include "media_processors/image/backends/raw_embedded_extractor.hpp"
#include "utils/subprocess_runner.hpp"
#include "utils/temp_paths.hpp"
#include "utils/tool_paths.hpp"
#include <Poco/Logger.h>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <vector>
#include <Magick++.h>
#if defined(HAVE_OPENCV)
#include <opencv2/opencv.hpp>
#endif

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
                else if (RawFileDetector::DetectFormatFromMagicBytes(source_path) == ImageFormat::TIFF)
                {
                    // No usable embedded image (e.g. lossy/JXL-compressed
                    // Lightroom DNGs whose thumbs LibRaw hands back as JXL,
                    // which nothing downstream decodes). TIFF-magic RAWs still
                    // carry a plain preview IFD that magick's tiff reader can
                    // decode — far better than the bare-RAW delegate, which
                    // needs the same LibRaw that just failed (or an external
                    // tool like darktable-cli that may not exist).
                    read_path = "tiff:" + source_path;
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

#if defined(HAVE_OPENCV)
        /// MP-1: in-process thumbnail for JPEG/PNG inputs — no subprocess, no
        /// temp output file. Decodes at 1/4 resolution when the source is
        /// comfortably larger than the thumbnail (libjpeg scaled decode),
        /// resizes to fit `size` (matching magick's "SxS" geometry), and
        /// encodes a metadata-free JPEG. EXIF orientation is applied by
        /// cv::imread. Returns false on any failure — the caller falls back
        /// to the magick subprocess path.
        bool encodeThumbnailWithOpenCv(const std::string &path,
                                       int size,
                                       int quality,
                                       Magick::Blob &blob,
                                       Poco::Logger &logger)
        {
            try
            {
                cv::setNumThreads(0);
                cv::Mat img;
                if (size > 0)
                {
                    img = cv::imread(path, cv::IMREAD_REDUCED_COLOR_4);
                    if (!img.empty() && std::min(img.rows, img.cols) < size)
                        img = cv::Mat();
                }
                if (img.empty())
                    img = cv::imread(path, cv::IMREAD_COLOR);
                if (img.empty())
                    return false;

                if (size > 0)
                {
                    double scale = std::min(static_cast<double>(size) / img.cols,
                                            static_cast<double>(size) / img.rows);
                    int nw = std::max(1, static_cast<int>(std::lround(img.cols * scale)));
                    int nh = std::max(1, static_cast<int>(std::lround(img.rows * scale)));
                    if (nw != img.cols || nh != img.rows)
                        cv::resize(img, img, cv::Size(nw, nh), 0, 0,
                                   scale < 1.0 ? cv::INTER_AREA : cv::INTER_LINEAR);
                }

                std::vector<uchar> encoded;
                if (!cv::imencode(".jpg", img, encoded, {cv::IMWRITE_JPEG_QUALITY, quality}) || encoded.empty())
                    return false;
                blob.update(encoded.data(), encoded.size());
                logger.debug("Generated thumbnail in-process (OpenCV): %s (%zu bytes)", path, encoded.size());
                return true;
            }
            catch (...)
            {
                return false;
            }
        }
#endif

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

            // MP-3: the native-resolution sentinel bypasses size snapping —
            // it is for the full-res viewer paths only, never cached.
            if (size != kNativeResolution && !isValidSize(size))
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

            // MP-3: the native-resolution sentinel bypasses size snapping —
            // it is for the full-res viewer paths only, never cached.
            if (size != kNativeResolution && !isValidSize(size))
            {
                logger.warning("Invalid thumbnail size %d requested, using closest valid size", size);
                size = getClosestValidSize(size);
            }

            TempFileCleanup cleanup;
            std::string read_path;
            if (!resolveInputPath(source_path, read_path, cleanup, logger))
                return false;

#if defined(HAVE_OPENCV)
            // MP-1: JPEG/PNG inputs (previews, RAW-embedded JPEGs, ordinary
            // photos — the overwhelming majority) encode in-process; magick
            // remains the fallback and the path for TIFF/HEIC/exotics.
            {
                std::string plain_path = read_path;
                bool opencv_ok_format = false;
                if (plain_path.rfind("jpeg:", 0) == 0)
                {
                    plain_path = plain_path.substr(5);
                    opencv_ok_format = true;
                }
                else if (plain_path.rfind("png:", 0) == 0)
                {
                    plain_path = plain_path.substr(4);
                    opencv_ok_format = true;
                }
                if (opencv_ok_format && encodeThumbnailWithOpenCv(plain_path, size, quality, blob, logger))
                    return true;
            }
#endif

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
        // MP-3: 0 is no longer a cached-thumbnail size — small/zero requests
        // snap to the smallest real size instead of a full-resolution render.
        // (The live bug: the gRPC default size=80 normalized to 0 and cached
        // full-resolution files forever.)
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
