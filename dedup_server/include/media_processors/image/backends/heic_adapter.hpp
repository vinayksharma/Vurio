#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace MediaDedup
{
    class HeicAdapter
    {
    public:
        /**
         * @brief Transcodes an Apple HEIC image to standard JPEG using the macOS `sips` command.
         * 
         * @param file_path The path to the HEIC file.
         * @param jpeg_data The vector to fill with JPEG binary data.
         * @return true if successful, false otherwise.
         */
        static bool TranscodeToJpeg(const std::string &file_path, std::vector<std::uint8_t> &jpeg_data);
    };
}
