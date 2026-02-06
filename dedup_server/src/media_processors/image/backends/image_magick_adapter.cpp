#include "media_processors/image/backends/image_magick_adapter.hpp"
#include "media_processors/image/backends/image_magick_transcoder.hpp"
#include <Poco/Logger.h>
#include <Magick++.h>
#include <MagickCore/MagickCore.h>
#include <sstream>
#include <filesystem>

namespace MediaDedup
{

    bool ImageMagickAdapter::TranscodeToJpeg(const std::string &file_path,
                                             std::vector<std::uint8_t> &jpeg_data)
    {
        Poco::Logger &log = Poco::Logger::get("ImageMagickAdapter");

        // Early validation for empty or invalid file paths
        if (file_path.empty())
        {
            log.warning("Empty file path provided for transcoding");
            jpeg_data.clear();
            return false;
        }

        // Validate file exists and is readable
        if (!std::filesystem::exists(file_path))
        {
            log.error("File does not exist: %s", file_path);
            jpeg_data.clear();
            return false;
        }

        if (!std::filesystem::is_regular_file(file_path))
        {
            log.error("Path is not a regular file: %s", file_path);
            jpeg_data.clear();
            return false;
        }

        // Create per-thread ImageMagick transcoder instance (RAII)
        // This eliminates the need for mutex locks and enables parallel processing
        ImageMagickTranscoder transcoder;

        if (!transcoder.isValid())
        {
            log.error("Failed to create ImageMagick transcoder instance for file: %s", file_path);
            jpeg_data.clear();
            return false;
        }

        // Use the RAII transcoder for thread-safe parallel processing
        bool result = transcoder.transcodeToJpeg(file_path, jpeg_data);
        if (!result)
        {
            log.warning("TranscodeToJpeg failed for file: %s", file_path);
        }
        return result;
    }
}
