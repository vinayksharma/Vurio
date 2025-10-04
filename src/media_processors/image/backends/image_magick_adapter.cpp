#include "media_processors/image/backends/image_magick_adapter.hpp"
#include <Poco/Logger.h>
#include <Magick++.h>
#include <MagickCore/MagickCore.h>
#include <sstream>
#include <filesystem>
#include <mutex>

namespace MediaDedup
{
    // Static initialization flag to ensure ImageMagick++ is initialized only once
    static std::once_flag magick_initialized;

    // Mutex to protect ImageMagick operations (not fully thread-safe)
    static std::mutex magick_mutex;

    bool ImageMagickAdapter::TranscodeToTiff(const std::string &file_path,
                                             std::vector<std::uint8_t> &tiff_data)
    {
        Poco::Logger &log = Poco::Logger::get("ImageMagickAdapter");

        // Early validation for empty or invalid file paths
        if (file_path.empty())
        {
            log.warning("Empty file path provided for transcoding");
            tiff_data.clear();
            return false;
        }

        // Validate file exists and is readable
        if (!std::filesystem::exists(file_path))
        {
            log.error("File does not exist: %s", file_path);
            tiff_data.clear();
            return false;
        }

        if (!std::filesystem::is_regular_file(file_path))
        {
            log.error("Path is not a regular file: %s", file_path);
            tiff_data.clear();
            return false;
        }

        try
        {
            // Initialize Magick++ once globally (thread-safe)
            // This must be called before any ImageMagick++ operations
            try
            {
                std::call_once(magick_initialized, []()
                               { 
                                   try {
                                       Magick::InitializeMagick(nullptr);
                                       
                                       // Set resource limits once during initialization to prevent memory exhaustion
                                       // Use more conservative limits to prevent system memory pressure
                                       MagickCore::SetMagickResourceLimit(MagickCore::MemoryResource, 512 * 1024 * 1024); // 512MB
                                       MagickCore::SetMagickResourceLimit(MagickCore::DiskResource, 1024 * 1024 * 1024);  // 1GB
                                       MagickCore::SetMagickResourceLimit(MagickCore::MapResource, 1024 * 1024 * 1024);   // 1GB
                                       MagickCore::SetMagickResourceLimit(MagickCore::WidthResource, 8192);               // Max width
                                       MagickCore::SetMagickResourceLimit(MagickCore::HeightResource, 8192);              // Max height
                                   } catch (...) {
                                       // Prevent any ImageMagick initialization exceptions from propagating
                                   } });
                log.debug("ImageMagick++ initialization verified");
            }
            catch (const Magick::Exception &e)
            {
                log.error("Failed to initialize ImageMagick++: %s", e.what());
                tiff_data.clear();
                return false;
            }
            catch (const std::exception &e)
            {
                log.error("Failed to initialize ImageMagick++ (std exception): %s", e.what());
                tiff_data.clear();
                return false;
            }
            catch (...)
            {
                log.error("Failed to initialize ImageMagick++ (unknown exception)");
                tiff_data.clear();
                return false;
            }

            log.information("Transcoding image: %s", file_path);

            // Lock to protect ImageMagick operations (not fully thread-safe)
            std::lock_guard<std::mutex> lock(magick_mutex);

            // Create independent Magick++ image instance
            Magick::Image image;

            // Read the raw image file with error handling
            try
            {
                image.read(file_path);
                log.debug("Successfully read image file: %s", file_path);
            }
            catch (const Magick::Exception &e)
            {
                log.error("Failed to read image file %s: %s", file_path, e.what());
                tiff_data.clear();
                return false;
            }
            catch (const std::exception &e)
            {
                log.error("Failed to read image file %s (std exception): %s", file_path, e.what());
                tiff_data.clear();
                return false;
            }
            catch (...)
            {
                log.error("Failed to read image file %s (unknown exception)", file_path);
                tiff_data.clear();
                return false;
            }

            // Validate image properties before processing
            if (image.columns() == 0 || image.rows() == 0)
            {
                log.error("Invalid image dimensions for %s: %zux%zu", file_path, image.columns(), image.rows());
                tiff_data.clear();
                return false;
            }

            log.debug("Image properties - size: %zux%zu, depth: %zu, colorspace: %d",
                      image.columns(), image.rows(), image.depth(), image.colorSpace());

            // Convert to 8-bit RGB if needed
            try
            {
                if (image.depth() > 8)
                {
                    image.depth(8);
                    log.debug("Converted image depth to 8-bit");
                }

                // Ensure RGB color space
                if (image.colorSpace() != Magick::RGBColorspace)
                {
                    image.colorSpace(Magick::RGBColorspace);
                    log.debug("Converted image to RGB colorspace");
                }
            }
            catch (const Magick::Exception &e)
            {
                log.error("Failed to convert image properties for %s: %s", file_path, e.what());
                tiff_data.clear();
                return false;
            }
            catch (const std::exception &e)
            {
                log.error("Failed to convert image properties for %s (std exception): %s", file_path, e.what());
                tiff_data.clear();
                return false;
            }
            catch (...)
            {
                log.error("Failed to convert image properties for %s (unknown exception)", file_path);
                tiff_data.clear();
                return false;
            }

            // Set TIFF format and compression for OpenCV compatibility
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
                    log.debug("Converted colorspace to RGB for OpenCV compatibility");
                }

                // Additional TIFF options for better OpenCV compatibility
                image.defineValue("tiff", "endian", "lsb");       // Little-endian byte order
                image.defineValue("tiff", "planar", "contig");    // Contiguous planar configuration
                image.defineValue("tiff", "rows-per-strip", "1"); // One row per strip for better compatibility

                log.debug("Set image format to TIFF (8-bit, RGB, no compression, LSB, contiguous) for OpenCV compatibility");
            }
            catch (const Magick::Exception &e)
            {
                log.error("Failed to set image format for %s: %s", file_path, e.what());
                tiff_data.clear();
                return false;
            }
            catch (const std::exception &e)
            {
                log.error("Failed to set image format for %s (std exception): %s", file_path, e.what());
                tiff_data.clear();
                return false;
            }
            catch (...)
            {
                log.error("Failed to set image format for %s (unknown exception)", file_path);
                tiff_data.clear();
                return false;
            }

            // Write to memory buffer
            try
            {
                Magick::Blob blob;
                image.write(&blob);

                // Validate blob data
                if (blob.length() == 0)
                {
                    log.error("Generated empty TIFF data for %s", file_path);
                    tiff_data.clear();
                    return false;
                }

                // Copy data to output vector
                const std::uint8_t *data = static_cast<const std::uint8_t *>(blob.data());
                size_t data_size = blob.length();

                tiff_data.assign(data, data + data_size);

                log.information("Successfully transcoded image: %s, size: %zu bytes", file_path, data_size);
                return true;
            }
            catch (const Magick::Exception &e)
            {
                log.error("Failed to write image to memory for %s: %s", file_path, e.what());
                tiff_data.clear();
                return false;
            }
            catch (const std::exception &e)
            {
                log.error("Failed to write image to memory for %s (std exception): %s", file_path, e.what());
                tiff_data.clear();
                return false;
            }
            catch (...)
            {
                log.error("Failed to write image to memory for %s (unknown exception)", file_path);
                tiff_data.clear();
                return false;
            }
        }
        catch (const Magick::Exception &e)
        {
            log.error("Failed to transcode image: %s, error: %s", file_path, e.what());
            tiff_data.clear();
            return false;
        }
        catch (const std::exception &e)
        {
            log.error("Failed to transcode image: %s, error: %s", file_path, e.what());
            tiff_data.clear();
            return false;
        }
        catch (...)
        {
            log.error("Failed to transcode image: %s, unknown error", file_path);
            tiff_data.clear();
            return false;
        }
    }
}
