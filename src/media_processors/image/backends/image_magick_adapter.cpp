#include "media_processors/image/backends/image_magick_adapter.hpp"
#include <Poco/Logger.h>
#include <Magick++.h>
#include <sstream>

namespace MediaDedup
{
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

        try
        {
            // Initialize Magick++ for this thread (thread-safe)
            Magick::InitializeMagick(nullptr);

            log.information("Transcoding image: %s", file_path);

            // Create independent Magick++ image instance
            Magick::Image image;

            // Read the raw image file
            image.read(file_path);

            // Convert to 8-bit RGB if needed
            if (image.depth() > 8)
            {
                image.depth(8);
            }

            // Ensure RGB color space
            if (image.colorSpace() != Magick::RGBColorspace)
            {
                image.colorSpace(Magick::RGBColorspace);
            }

            // Set TIFF format and compression
            image.magick("TIFF");
            image.compressType(Magick::LZWCompression);

            // Write to memory buffer
            Magick::Blob blob;
            image.write(&blob);

            // Copy data to output vector
            const std::uint8_t *data = static_cast<const std::uint8_t *>(blob.data());
            size_t data_size = blob.length();

            tiff_data.assign(data, data + data_size);

            log.debug("Successfully transcoded image: %s, size: %zu bytes", file_path, data_size);
            return true;
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
