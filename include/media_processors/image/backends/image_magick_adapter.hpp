#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace MediaDedup
{
    /**
     * @brief ImageMagick adapter for transcoding raw image files to JPEG format
     *
     * This class provides thread-safe static methods for transcoding raw image files
     * (ARW, CR2, DNG, etc.) to 8-bit JPEG format in memory using ImageMagick++.
     * Each method call creates independent Magick++ instances for thread safety.
     */
    class ImageMagickAdapter
    {
    public:
        /**
         * @brief Transcode a raw image file to JPEG format in memory
         *
         * @param file_path Fully qualified path to the raw image file
         * @param jpeg_data Output vector containing the transcoded JPEG data
         * @return true if transcoding succeeded, false otherwise
         *
         * Thread-safe: Each call creates independent Magick++ instances
         * Logs info message on successful processing and error messages on failure
         */
        static bool TranscodeToJpeg(const std::string &file_path,
                                    std::vector<std::uint8_t> &jpeg_data);
    };
}
