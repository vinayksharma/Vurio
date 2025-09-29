#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace MediaDedup
{
    class DatabaseManager;

    struct BalancedPipelineConfig
    {
        int resize_long_edge = 1024;
        int max_keypoints = 1000;
    };

    class BalancedPipeline
    {
    public:
        static bool Run(const std::string &file_path,
                        const BalancedPipelineConfig &cfg,
                        DatabaseManager &db);

        static bool Run(const std::vector<std::uint8_t> &image_data,
                        const std::string &original_file_path,
                        const BalancedPipelineConfig &cfg,
                        DatabaseManager &db);
    };
}
