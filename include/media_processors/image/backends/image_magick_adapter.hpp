#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace MediaDedup
{
    /**
     * @brief ImageMagick adapter for transcoding raw image files to TIFF format
     *
     * This class provides thread-safe static methods for transcoding raw image files
     * (ARW, CR2, DNG, etc.) to 8-bit TIFF format in memory using ImageMagick++.
     * Each method call creates independent Magick++ instances for thread safety.
     */
    class ImageMagickAdapter
    {
    public:
        /**
         * @brief Transcode a raw image file to TIFF format in memory
         *
         * @param file_path Fully qualified path to the raw image file
         * @param tiff_data Output vector containing the transcoded TIFF data
         * @return true if transcoding succeeded, false otherwise
         *
         * Thread-safe: Each call creates independent Magick++ instances
         * Logs info message on successful processing and error messages on failure
         */
        static bool TranscodeToTiff(const std::string &file_path,
                                    std::vector<std::uint8_t> &tiff_data);
    };
}
