#include "media_processors/image/backends/image_magick_transcoder.hpp"
#include "media_processors/image/backends/raw_file_detector.hpp"
#include "media_processors/image/backends/raw_embedded_extractor.hpp"
#include "utils/subprocess_runner.hpp"
#include "utils/temp_paths.hpp"
#include "utils/tool_paths.hpp"
#include <MagickCore/MagickCore.h>
#include <Poco/Logger.h>
#include <filesystem>
#include <cstdlib>
#include <fstream>

namespace MediaDedup
{
    // Global initialization flag for ImageMagick (defined here, declared in header)
    std::once_flag magick_global_initialized;

    ImageMagickTranscoder::ImageMagickTranscoder()
        : valid_(false), logger_(Poco::Logger::get("ImageMagickTranscoder"))
    {
        valid_ = initialize();
        if (valid_)
        {
            logger_.debug("ImageMagickTranscoder instance created successfully");
        }
        else
        {
            logger_.error("Failed to create ImageMagickTranscoder instance");
        }
    }

    ImageMagickTranscoder::~ImageMagickTranscoder()
    {
        // ImageMagick resources are automatically cleaned up
        // when the Magick::Image object is destroyed
        logger_.debug("ImageMagickTranscoder instance destroyed");
    }

    bool ImageMagickTranscoder::initialize()
    {
        try
        {
            // Initialize ImageMagick globally (thread-safe, only called once)
            std::call_once(magick_global_initialized, []()
                           {
                try
                {
                    // CRITICAL: Disable ImageMagick assertions at environment level
                    // This prevents assertion failures in compiled code that can't be caught by handlers
                    ::setenv("MAGICK_DEBUG", "None", 1);  // Disable debugging assertions
                    
                    Magick::InitializeMagick(nullptr);
                    
                    // CRITICAL: Set error handlers to catch corruption before assertions trigger
                    // When ImageMagick encounters corrupted files, it can trigger fatal assertions.
                    // We replace default handlers with non-fatal ones that log but don't abort.
                    // Note: FatalErrorHandler requires noreturn attribute so we can't override it
                    // Instead we rely on MAGICK_DEBUG=None to disable assertions at compile/runtime level
                    
                    // Set regular error handler - just log, don't abort
                    MagickCore::SetErrorHandler([](const MagickCore::ExceptionType severity,
                                                   const char *reason,
                                                   const char *description) {
                        // Log errors but don't throw - let the transcoding continue
                        Poco::Logger::get("ImageMagick").error("ImageMagick Error [%d]: %s - %s",
                                                               static_cast<int>(severity),
                                                               reason ? reason : "unknown",
                                                               description ? description : "");
                    });
                    
                    // Set warning handler - just log, don't interrupt
                    MagickCore::SetWarningHandler([](const MagickCore::ExceptionType severity,
                                                     const char *reason,
                                                     const char *description) {
                        // Log warnings but continue processing
                        Poco::Logger::get("ImageMagick").debug("ImageMagick Warning [%d]: %s - %s",
                                                               static_cast<int>(severity),
                                                               reason ? reason : "unknown",
                                                               description ? description : "");
                    });
                    
                    // Set resource limits for RAW file processing
                    // ARW files can generate 100-200MB TIFFs, need more memory
                    MagickCore::SetMagickResourceLimit(MagickCore::MemoryResource, 2048ULL * 1024 * 1024);  // 2GB (increased for RAW)
                    MagickCore::SetMagickResourceLimit(MagickCore::DiskResource, 4096ULL * 1024 * 1024);    // 4GB
                    MagickCore::SetMagickResourceLimit(MagickCore::MapResource, 2048ULL * 1024 * 1024);      // 2GB
                    MagickCore::SetMagickResourceLimit(MagickCore::WidthResource, 16384);                 // Max width (8K sensors)
                    MagickCore::SetMagickResourceLimit(MagickCore::HeightResource, 16384);               // Max height
                    
                    // CRITICAL: Limit ImageMagick to 1 thread per instance!
                    // Since we run ImageMagick inside a ThreadPool (10+ parallel instances),
                    // allowing it to spawn its own OpenMP threads per-instance causes
                    // massive context-switching overhead and grinds the entire system to a halt.
                    MagickCore::SetMagickResourceLimit(MagickCore::ThreadResource, 1);
                    
                    Poco::Logger::get("ImageMagick").information("Initialized with MAGICK_DEBUG=None, 1 thread per instance, and custom ERROR/WARNING handlers to prevent crashes");
                }
                catch (...)
                {
                    // Prevent any ImageMagick initialization exceptions from propagating
                } });

            // Initialize this instance's image object with default properties
            // Don't set a specific format - let ImageMagick auto-detect when reading
            // Note: image_ is already initialized by the constructor, no need to call size()

            logger_.debug("ImageMagickTranscoder instance initialized successfully");
            return true;
        }
        catch (const Magick::Exception &e)
        {
            logger_.error("Failed to initialize ImageMagickTranscoder: %s", e.what());
            return false;
        }
        catch (const std::exception &e)
        {
            logger_.error("Failed to initialize ImageMagickTranscoder (std exception): %s", e.what());
            return false;
        }
        catch (...)
        {
            logger_.error("Failed to initialize ImageMagickTranscoder (unknown exception)");
            return false;
        }
    }

    std::vector<std::string> ImageMagickTranscoder::buildPreviewMagickArgs(const std::string &read_path,
                                                                           const std::string &output_path)
    {
        std::vector<std::string> args;
        // MP-1: for JPEG inputs, let libjpeg decode at the smallest scale
        // that still covers 2x the preview target instead of full
        // resolution (2-4x faster on large camera JPEGs). Must precede the
        // input argument to affect the decoder.
        if (read_path.rfind("jpeg:", 0) == 0)
        {
            const std::string hint = std::to_string(2 * MAX_PREVIEW_DIMENSION);
            args.push_back("-define");
            args.push_back("jpeg:size=" + hint + "x" + hint);
        }
        args.push_back(read_path);
        // Drop EXIF/XMP/ICC/IPTC profiles from the cached preview: metadata is
        // always read from the original file, never from the preview, so the
        // profiles are re-encoded dead weight. Placed right after the input so
        // every later operator works on the stripped image.
        args.push_back("-strip");
        args.push_back("-depth");
        args.push_back("8");
        args.push_back("-auto-orient");
        args.push_back("-colorspace");
        args.push_back("sRGB");
        args.push_back("-resize");
        args.push_back(std::to_string(MAX_PREVIEW_DIMENSION) + "x" + std::to_string(MAX_PREVIEW_DIMENSION) + ">");
        args.push_back("-quality");
        args.push_back("85");
        args.push_back("jpeg:" + output_path);
        return args;
    }

    bool ImageMagickTranscoder::transcodeToJpeg(const std::string &file_path, std::vector<std::uint8_t> &jpeg_data,
                                                int timeout_ms)
    {
        if (!valid_)
        {
            logger_.error("Transcoder instance is not valid");
            return false;
        }

        if (file_path.empty())
        {
            logger_.error("File path is empty");
            return false;
        }

        if (!std::filesystem::exists(file_path))
        {
            logger_.error("Source file does not exist: %s", file_path.c_str());
            return false;
        }

        // Fail fast on zero-filled/truncated copies — no decoder can read them.
        if (RawFileDetector::HasZeroedHeader(file_path))
        {
            logger_.error("Corrupted file (zeroed header), skipping transcode: %s", file_path.c_str());
            return false;
        }

        logger_.debug("Starting subprocess transcoding for file: %s", file_path.c_str());

        std::string temp_out_path = makeTempFilePath("magick_out", ".jpg");

        // Determine input prefix based on magic bytes
        ImageFormat format = RawFileDetector::DetectFormatFromMagicBytes(file_path);
        std::string read_path = file_path;
        // Temp file holding an extracted embedded JPEG; removed after the run.
        std::string embedded_temp_path;
        // Embedded JPEG below the QHD target: kept as a last-resort input for
        // when the full demosaic fails (e.g. a magick build without RAW
        // support) — a camera preview beats no preview at all.
        std::string small_embedded_path;

        // Don't prepend tiff: for RAW files even if they have TIFF magic bytes
        if (format == ImageFormat::TIFF && RawFileDetector::IsRawFile(file_path)) {
            format = ImageFormat::RAW;
        }

        if (format == ImageFormat::JPEG) {
            read_path = "jpeg:" + file_path;
        } else if (format == ImageFormat::PNG) {
            read_path = "png:" + file_path;
        } else if (format == ImageFormat::TIFF) {
            read_path = "tiff:" + file_path;
        } else if (format == ImageFormat::GIF) {
            read_path = file_path + "[0]";
        } else if (RawFileDetector::IsRawFile(file_path)) {
            // Consolidated RAW path (same decoder the thumbnail side uses):
            // prefer the camera's embedded JPEG when it can carry a QHD
            // preview; fall back to the slow full demosaic only when the
            // embedded image is absent or smaller than the output target.
            if (auto embedded = RawEmbeddedExtractor::extract(file_path))
            {
                if (embedded->longEdge() >= static_cast<int>(MAX_PREVIEW_DIMENSION))
                {
                    embedded_temp_path = embedded->jpeg_path;
                    read_path = "jpeg:" + embedded_temp_path;
                    logger_.debug("Using embedded JPEG (%dx%d) for RAW preview: %s",
                                  embedded->width, embedded->height, file_path.c_str());
                }
                else
                {
                    logger_.debug("Embedded JPEG too small (%dx%d < %d), full demosaic: %s",
                                  embedded->width, embedded->height,
                                  static_cast<int>(MAX_PREVIEW_DIMENSION), file_path.c_str());
                    small_embedded_path = embedded->jpeg_path;
                }
            }
        }

        std::string command = ToolPaths::magickPath();

        struct EmbeddedCleanup {
            std::string path;
            std::string small_path;
            ~EmbeddedCleanup() {
                std::error_code ec;
                if (!path.empty())
                    std::filesystem::remove(path, ec);
                if (!small_path.empty())
                    std::filesystem::remove(small_path, ec);
            }
        } embedded_cleanup{embedded_temp_path, small_embedded_path};

        try
        {
            auto runMagickTo = [&](const std::string &input) -> bool
            {
                std::vector<std::string> args = buildPreviewMagickArgs(input, temp_out_path);
                SubprocessResult run = runProcessWithTimeout(command, args, timeout_ms,
                                                             ToolPaths::magickResourceEnv());
                if (run.timed_out)
                {
                    logger_.error("magick timed out after %d ms for file: %s", timeout_ms, file_path);
                    return false;
                }
                if (run.exit_code != 0)
                {
                    logger_.error("magick command failed with exit code %d for file: %s", run.exit_code, file_path);
                    return false;
                }
                if (!std::filesystem::exists(temp_out_path))
                {
                    logger_.error("magick succeeded but output file is missing: %s", temp_out_path);
                    return false;
                }
                return true;
            };

            bool ran_ok = runMagickTo(read_path);
            if (!ran_ok && !small_embedded_path.empty())
            {
                // The demosaic path is broken or timed out; serve the camera's
                // own preview at its native resolution instead of failing (and
                // eventually negative-caching a perfectly readable file).
                logger_.warning("RAW decode failed, falling back to sub-QHD embedded JPEG for: %s", file_path);
                ran_ok = runMagickTo("jpeg:" + small_embedded_path);
            }
            if (!ran_ok)
            {
                if (std::filesystem::exists(temp_out_path)) std::filesystem::remove(temp_out_path);
                return false;
            }

            // Read output file
            std::ifstream file(temp_out_path, std::ios::binary | std::ios::ate);
            if (!file.is_open())
            {
                logger_.error("Failed to read the temporary JPEG file: %s", temp_out_path);
                std::filesystem::remove(temp_out_path);
                return false;
            }

            std::streamsize size = file.tellg();
            file.seekg(0, std::ios::beg);

            jpeg_data.resize(size);
            if (file.read(reinterpret_cast<char*>(jpeg_data.data()), size))
            {
                logger_.information("Successfully transcoded image to JPEG via subprocess: %s (%zu bytes)", file_path, static_cast<size_t>(size));
                std::filesystem::remove(temp_out_path);
                return true;
            }
            else
            {
                logger_.error("Failed to read data from temporary JPEG file: %s", temp_out_path);
                std::filesystem::remove(temp_out_path);
                return false;
            }
        }
        catch (const std::exception &e)
        {
            logger_.error("Exception executing magick on image file %s: %s", file_path, e.what());
            if (std::filesystem::exists(temp_out_path)) std::filesystem::remove(temp_out_path);
            return false;
        }
    }


}
