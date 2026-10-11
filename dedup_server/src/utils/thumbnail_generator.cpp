#include "utils/thumbnail_generator.hpp"
#include "adjustments/chain_engine.hpp"
#include "media_processors/image/backends/tiff_validator.hpp"
#include "media_processors/image/backends/raw_file_detector.hpp"
#include "media_processors/image/backends/raw_demosaic_renderer.hpp"
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
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

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

        /// Where the pixels for one render should come from.
        struct ResolvedInput
        {
            /// magick/OpenCV input spec (may carry a "jpeg:"/"png:"/"tiff:" prefix).
            std::string read_path;
            /// Set instead of `read_path` when the only adequate source is the
            /// RAW sensor data itself: the caller demosaics this in-process.
            std::string demosaic_source;
            /// Second chance if the demosaic fails — the camera's embedded
            /// preview. Better a small real image than no image at all.
            std::string fallback_read_path;
        };

        /// Shared front half of thumbnail generation: validates the source and
        /// resolves where to read pixels from. RAW files route through the
        /// single consolidated decoder (RawEmbeddedExtractor); everything else
        /// gets a magic-bytes format prefix. Returns false when the source is
        /// unusable.
        ///
        /// `size` is the caller's longest-edge target (kNativeResolution for the
        /// full-resolution viewer path). It decides whether a sub-full-size
        /// embedded RAW preview is good enough: at thumbnail sizes any embedded
        /// image is, at native resolution it is not — see the RAW branch.
        bool resolveInputPath(const std::string &source_path,
                              int size,
                              ResolvedInput &input,
                              TempFileCleanup &cleanup,
                              Poco::Logger &logger)
        {
            std::string &read_path = input.read_path;
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
                // Consolidated RAW path: the camera's embedded JPEG covers
                // every cached size cheaply. Developing the sensor data is
                // reserved for native resolution, where nothing else will do.
                if (auto embedded = RawEmbeddedExtractor::extract(source_path))
                {
                    cleanup.path = embedded->jpeg_path;

                    // At native resolution the embedded preview is only an
                    // acceptable answer when it already carries the sensor's
                    // full resolution. Often it does not — 1616x1080 out of
                    // 5496x3672 is an ordinary Sony ARW — and taking it anyway
                    // made the viewer's zoom-time "full resolution" upgrade
                    // hand back an image SMALLER than the 2560px QHD preview it
                    // was replacing. Develop the sensor data instead.
                    if (size != ThumbnailGenerator::kNativeResolution || embedded->isFullSize())
                    {
                        read_path = "jpeg:" + embedded->jpeg_path;
                    }
                    else
                    {
                        logger.debug("Embedded preview %dx%d is sub-full-size (sensor %dx%d), "
                                     "demosaicing for native resolution: %s",
                                     embedded->width, embedded->height,
                                     embedded->source_width, embedded->source_height, source_path);
                        read_path.clear();
                        input.demosaic_source = source_path;
                        input.fallback_read_path = "jpeg:" + embedded->jpeg_path;
                    }
                }
                else
                {
                    // No usable embedded image (e.g. lossy/JXL-compressed
                    // Lightroom DNGs whose thumbs LibRaw hands back as JXL,
                    // which nothing downstream decodes). TIFF-magic RAWs still
                    // carry a plain preview IFD that magick's tiff reader can
                    // decode — far better than the bare-RAW delegate, which
                    // needs an external tool like darktable-cli that may not
                    // exist. At native resolution that preview IFD is still
                    // only a preview, so develop the sensor data and keep the
                    // IFD as the fallback.
                    std::string tiff_read_path;
                    if (RawFileDetector::DetectFormatFromMagicBytes(source_path) == ImageFormat::TIFF)
                        tiff_read_path = "tiff:" + source_path;

                    if (size == ThumbnailGenerator::kNativeResolution)
                    {
                        read_path.clear();
                        input.demosaic_source = source_path;
                        input.fallback_read_path = tiff_read_path;
                    }
                    else if (!tiff_read_path.empty())
                    {
                        read_path = tiff_read_path;
                    }
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

        /// CHAIN-1: decode a JPEG/PNG base no larger than the engine needs.
        /// libjpeg's scaled decode (1/8, 1/4, 1/2) is tried coarse to fine;
        /// the geometry is normalised, so the ratio of rendered size to base
        /// size is the same at every level and tells whether a level still
        /// reaches `target` on the output's longest side. `target` <= 0 is
        /// native: the full decode.
        // CHAIN-5: a thumbnail or full render computes a missing detected
        // mask inline (it is off the UI thread already, docs §5.6).
        Adjustments::RenderPlan::RenderOptions editOptions(const std::string &source_path)
        {
            Adjustments::RenderPlan::RenderOptions options;
            options.file_path = source_path;
            options.inline_masks = true;
            return options;
        }

        cv::Mat decodeBaseForTarget(const std::string &path, const Adjustments::RenderPlan &plan, int target)
        {
            if (target > 0)
            {
                for (int flag : {cv::IMREAD_REDUCED_COLOR_8, cv::IMREAD_REDUCED_COLOR_4, cv::IMREAD_REDUCED_COLOR_2})
                {
                    cv::Mat img;
                    try
                    {
                        img = cv::imread(path, flag);
                    }
                    catch (...)
                    {
                        img = cv::Mat();
                    }
                    if (img.empty())
                        break;
                    Adjustments::Size base{static_cast<double>(img.cols), static_cast<double>(img.rows)};
                    Adjustments::Size out = plan.displaySize(base);
                    if (std::max(out.width, out.height) >= target)
                        return img;
                }
            }
            return Adjustments::ChainEngine::decodeBgr8(path, 0);
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

    // CL-2: the file-output generate() variant deleted (production uses
    // generateToBlob; the last file-variant callers died with the HTTP surface).

    bool ThumbnailGenerator::generateToBlob(const std::string &source_path,
                                             int size,
                                             Magick::Blob &blob,
                                             int quality,
                                             int timeout_ms,
                                             const Adjustments::RenderPlan &plan)
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
            ResolvedInput input;
            if (!resolveInputPath(source_path, size, input, cleanup, logger))
                return false;

            // Native resolution out of RAW sensor data: develop it in-process.
            // This IS the requested output — full resolution, encoded once at
            // the requested quality — so nothing below needs to run.
            TempFileCleanup demosaic_cleanup;
            if (!input.demosaic_source.empty())
            {
                if (RawDemosaicRenderer::renderToBlob(input.demosaic_source, quality, timeout_ms, blob))
                {
                    if (plan.isIdentity())
                        return true;
                    // CHAIN-1: the developed RAW is the engine's base.
                    std::vector<uint8_t> developed(static_cast<const uint8_t *>(blob.data()),
                                                   static_cast<const uint8_t *>(blob.data()) + blob.length());
                    cv::Mat base = Adjustments::ChainEngine::decodeBgr8(developed);
                    std::vector<uint8_t> jpeg;
                    if (!Adjustments::ChainEngine::renderToJpeg(base, plan, size, quality, jpeg, nullptr, nullptr, editOptions(source_path)))
                    {
                        logger.error("Edit chain failed on the developed RAW: %s", source_path);
                        return false;
                    }
                    blob.update(jpeg.data(), jpeg.size());
                    return true;
                }
                else
                {
                if (input.fallback_read_path.empty())
                    return false;
                // A small real image beats none, and beats letting a readable
                // file drift into the negative cache.
                logger.warning("RAW demosaic failed, falling back to the embedded preview for: %s", source_path);
                input.read_path = input.fallback_read_path;
                }
            }

            const std::string &read_path = input.read_path;

            // CHAIN-1 (docs/todo_tool_chain.md §5.3): an edited file is
            // rendered by the engine on a decoded base. JPEG/PNG decode
            // in-process (with libjpeg's scaled decode when the working size
            // allows); everything else is decoded once by magick to a temp
            // PNG at native resolution, which is the one subprocess the
            // exotic formats always cost.
            if (!plan.isIdentity())
            {
                cv::Mat base;
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
                if (opencv_ok_format)
                    base = decodeBaseForTarget(plain_path, plan, size);
                if (base.empty())
                {
                    TempFileCleanup base_cleanup;
                    base_cleanup.path = makeTempFilePath("adj_base", ".png");
                    std::vector<std::string> args = {read_path, "-strip", "-depth", "8", "-auto-orient",
                                                     "-colorspace", "sRGB", "png:" + base_cleanup.path};
                    SubprocessResult result = runProcessWithTimeout(ToolPaths::magickPath(), args, timeout_ms,
                                                                    ToolPaths::magickResourceEnv());
                    if (result.timed_out || result.exit_code != 0)
                    {
                        logger.error("magick base decode failed for the edit chain: %s", source_path);
                        return false;
                    }
                    base = Adjustments::ChainEngine::decodeBgr8(base_cleanup.path);
                }
                if (base.empty())
                {
                    logger.error("Base image could not be decoded for the edit chain: %s", source_path);
                    return false;
                }
                std::vector<uint8_t> jpeg;
                if (!Adjustments::ChainEngine::renderToJpeg(base, plan, size, quality, jpeg, nullptr, nullptr, editOptions(source_path)))
                {
                    logger.error("Edit chain render failed for: %s", source_path);
                    return false;
                }
                blob.update(jpeg.data(), jpeg.size());
                logger.debug("Generated edited render in-process: %s (%zu bytes)", source_path, jpeg.size());
                return true;
            }

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
