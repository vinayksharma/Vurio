#include "media_processors/image/backends/image_magick_transcoder.hpp"
#include <MagickCore/MagickCore.h>
#include <Poco/Logger.h>
#include <filesystem>

namespace MediaDedup
{
    // Global initialization flag for ImageMagick
    static std::once_flag magick_global_initialized;

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
                    Magick::InitializeMagick(nullptr);
                    
                    // Set conservative resource limits to prevent memory exhaustion
                    MagickCore::SetMagickResourceLimit(MagickCore::MemoryResource, 512 * 1024 * 1024);  // 512MB
                    MagickCore::SetMagickResourceLimit(MagickCore::DiskResource, 1024 * 1024 * 1024);   // 1GB
                    MagickCore::SetMagickResourceLimit(MagickCore::MapResource, 1024 * 1024 * 1024);     // 1GB
                    MagickCore::SetMagickResourceLimit(MagickCore::WidthResource, 8192);                  // Max width
                    MagickCore::SetMagickResourceLimit(MagickCore::HeightResource, 8192);                // Max height
                }
                catch (...)
                {
                    // Prevent any ImageMagick initialization exceptions from propagating
                }
            });

            // Initialize this instance's image object with default properties
            // Don't set a specific format - let ImageMagick auto-detect when reading
            image_.size("100x100");  // Set a default size
            
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

    bool ImageMagickTranscoder::transcodeToTiff(const std::string &file_path, std::vector<std::uint8_t> &tiff_data)
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
            image_.read(file_path);
            logger_.debug("Successfully read image file: %s", file_path);

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

            // Set TIFF format options for OpenCV compatibility
            setTiffFormatOptions(image_);

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
            tiff_data.assign(data, data + data_size);

            logger_.information("Successfully transcoded image: %s, size: %zu bytes", file_path, data_size);
            return true;
        }
        catch (const Magick::Exception &e)
        {
            logger_.error("Failed to transcode image %s: %s", file_path, e.what());
            tiff_data.clear();
            return false;
        }
        catch (const std::exception &e)
        {
            logger_.error("Failed to transcode image %s (std exception): %s", file_path, e.what());
            tiff_data.clear();
            return false;
        }
        catch (...)
        {
            logger_.error("Failed to transcode image %s (unknown exception)", file_path);
            tiff_data.clear();
            return false;
        }
    }

    void ImageMagickTranscoder::configureImageProperties(Magick::Image& image)
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

    void ImageMagickTranscoder::setTiffFormatOptions(Magick::Image& image)
    {
        try
        {
            image.magick("TIFF");
            // Use no compression for better OpenCV compatibility
            image.compressType(Magick::NoCompression);
            // Ensure 8-bit depth for OpenCV compatibility
            image.depth(8);
            // Set colorspace to RGB for better compatibility
            if (image.colorSpace() != Magick::RGBColorspace)
            {
                image.colorSpace(Magick::RGBColorspace);
                logger_.debug("Converted colorspace to RGB for OpenCV compatibility");
            }

            // Additional TIFF options for better OpenCV compatibility
            image.defineValue("tiff", "endian", "lsb");       // Little-endian byte order
            image.defineValue("tiff", "planar", "contig");    // Contiguous planar configuration
            image.defineValue("tiff", "rows-per-strip", "1"); // One row per strip for better compatibility

            logger_.debug("Set image format to TIFF (8-bit, RGB, no compression, LSB, contiguous) for OpenCV compatibility");
        }
        catch (const Magick::Exception &e)
        {
            logger_.error("Failed to set TIFF format options: %s", e.what());
            throw;
        }
        catch (const std::exception &e)
        {
            logger_.error("Failed to set TIFF format options (std exception): %s", e.what());
            throw;
        }
        catch (...)
        {
            logger_.error("Failed to set TIFF format options (unknown exception)");
            throw;
        }
    }
}
