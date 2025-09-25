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
    };
}
