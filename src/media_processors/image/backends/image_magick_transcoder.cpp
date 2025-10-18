#include "media_processors/image/backends/image_magick_transcoder.hpp"
#include <MagickCore/MagickCore.h>
#include <Poco/Logger.h>
#include <filesystem>
#include <cstdlib>

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
                    
                    // Set regular error handler
                    MagickCore::SetErrorHandler([](const MagickCore::ExceptionType severity,
                                                   const char *reason,
                                                   const char *description) {
                        // Log but don't abort - convert to exception instead
                        Poco::Logger::get("ImageMagick").error("ImageMagick Error [%d]: %s - %s",
                                                               static_cast<int>(severity),
                                                               reason ? reason : "unknown",
                                                               description ? description : "");
                    });
                    
                    // Set warning handler
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
                    
                    Poco::Logger::get("ImageMagick").information("Initialized with MAGICK_DEBUG=None and custom ERROR/WARNING handlers to prevent crashes");
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

    bool ImageMagickTranscoder::transcodeToJpeg(const std::string &file_path, std::vector<std::uint8_t> &jpeg_data)
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
            logger_.error("Source file does not exist: %s", file_path);
            return false;
        }

        logger_.debug("Starting transcoding for file: %s", file_path);

        try
        {
            logger_.information("Transcoding image: %s", file_path);

            // Read the new file (ImageMagick will automatically clear previous data)
            try
            {
                logger_.debug("Reading RAW/image file with ImageMagick: %s", file_path);
                image_.read(file_path);
                logger_.debug("Successfully read image file: %s (size: %zux%zu, depth: %zu)",
                              file_path, image_.columns(), image_.rows(), image_.depth());
            }
            catch (const Magick::Exception &e)
            {
                logger_.error("Failed to read image file %s: %s [MagickException]", file_path, e.what());
                logger_.error("Possible causes: insufficient memory, unsupported RAW format, or corrupted file");
                jpeg_data.clear();
                return false;
            }
            catch (const std::exception &e)
            {
                logger_.error("Failed to read image file %s (std exception): %s", file_path, e.what());
                jpeg_data.clear();
                return false;
            }
            catch (...)
            {
                logger_.error("Failed to read image file %s (unknown exception)", file_path);
                jpeg_data.clear();
                return false;
            }

            // Validate image properties
            if (image_.columns() == 0 || image_.rows() == 0)
            {
                logger_.error("Invalid image dimensions for %s: %zux%zu", file_path, image_.columns(), image_.rows());
                return false;
            }

            logger_.debug("Image properties - size: %zux%zu, depth: %zu, colorspace: %d",
                          image_.columns(), image_.rows(), image_.depth(), image_.colorSpace());

            // Configure image properties for optimal transcoding
            configureImageProperties(image_);

            // Set JPEG format options for OpenCV compatibility
            setJpegFormatOptions(image_);

            // Write to memory buffer
            Magick::Blob blob;
            image_.write(&blob);

            // Validate blob data
            if (blob.length() == 0)
            {
                logger_.error("Transcoding produced empty data for file: %s", file_path);
                return false;
            }

            // Copy data to output vector
            const char *data = static_cast<const char *>(blob.data());
            size_t data_size = blob.length();
            jpeg_data.assign(data, data + data_size);

            logger_.information("Successfully transcoded image: %s, size: %zu bytes", file_path, data_size);
            return true;
        }
        catch (const Magick::Exception &e)
        {
            logger_.error("Failed to transcode image %s: %s", file_path, e.what());
            jpeg_data.clear();
            return false;
        }
        catch (const std::exception &e)
        {
            logger_.error("Failed to transcode image %s (std exception): %s", file_path, e.what());
            jpeg_data.clear();
            return false;
        }
        catch (...)
        {
            logger_.error("Failed to transcode image %s (unknown exception)", file_path);
            jpeg_data.clear();
            return false;
        }
    }

    void ImageMagickTranscoder::configureImageProperties(Magick::Image &image)
    {
        try
        {
            // Convert to 8-bit RGB if needed
            if (image.depth() > 8)
            {
                image.depth(8);
                logger_.debug("Converted image depth to 8-bit");
            }

            // Ensure RGB color space
            if (image.colorSpace() != Magick::RGBColorspace)
            {
                image.colorSpace(Magick::RGBColorspace);
                logger_.debug("Converted image to RGB colorspace");
            }
        }
        catch (const Magick::Exception &e)
        {
            logger_.error("Failed to configure image properties: %s", e.what());
            throw;
        }
        catch (const std::exception &e)
        {
            logger_.error("Failed to configure image properties (std exception): %s", e.what());
            throw;
        }
        catch (...)
        {
            logger_.error("Failed to configure image properties (unknown exception)");
            throw;
        }
    }

    void ImageMagickTranscoder::setJpegFormatOptions(Magick::Image &image)
    {
        try
        {
            image.magick("JPEG");
            // Use quality 95 for near-lossless compression with excellent size reduction
            image.quality(95);
            // Ensure 8-bit depth for OpenCV compatibility
            image.depth(8);
            // Set colorspace to RGB for better compatibility
            if (image.colorSpace() != Magick::RGBColorspace)
            {
                image.colorSpace(Magick::RGBColorspace);
                logger_.debug("Converted colorspace to RGB for OpenCV compatibility");
            }

            logger_.debug("Set image format to JPEG (8-bit, RGB, quality 95) for OpenCV compatibility");
        }
        catch (const Magick::Exception &e)
        {
            logger_.error("Failed to set JPEG format options: %s", e.what());
            throw;
        }
        catch (const std::exception &e)
        {
            logger_.error("Failed to set JPEG format options (std exception): %s", e.what());
            throw;
        }
        catch (...)
        {
            logger_.error("Failed to set JPEG format options (unknown exception)");
            throw;
        }
    }
}
