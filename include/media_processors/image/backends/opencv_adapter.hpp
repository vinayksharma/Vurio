#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace MediaDedup
{
    struct OpenCvHashResult
    {
        std::vector<std::uint8_t> phash64; // 8 bytes
        int thumb_w = 0;
        int thumb_h = 0;
    };

    class OpenCvAdapter
    {
    public:
        static bool ComputePhash(const std::string &file_path,
                                 int thumb_size,
                                 OpenCvHashResult &out);

        // Memory-based version for processing image data directly from memory
        static bool ComputePhash(const std::vector<std::uint8_t> &image_data,
                                 int thumb_size,
                                 OpenCvHashResult &out);
    };
}
