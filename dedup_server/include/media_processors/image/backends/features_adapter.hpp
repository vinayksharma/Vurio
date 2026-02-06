#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace MediaDedup
{
    // Serialized layout (little-endian):
    // [u32 num_keypoints][u32 desc_rows][u32 desc_cols][u32 desc_type]
    // [keypoints: num_keypoints * (float x,y,size,angle; float response; int octave; int class_id)]
    // [descriptors raw bytes]
    class FeaturesAdapter
    {
    public:
        static bool ExtractFeaturesToBlob(const std::string &file_path,
                                          int resize_long_edge,
                                          int max_keypoints,
                                          std::vector<std::uint8_t> &out_blob);

        static bool ExtractFeaturesToBlob(const std::vector<std::uint8_t> &image_data,
                                          int resize_long_edge,
                                          int max_keypoints,
                                          std::vector<std::uint8_t> &out_blob);
    };
}
