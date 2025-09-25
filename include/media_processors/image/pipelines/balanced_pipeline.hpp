#pragma once

#include <string>

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
    };
}


