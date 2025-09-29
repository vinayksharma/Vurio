#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <memory>

namespace MediaDedup
{
    class DatabaseManager;
    class UnifiedObservableConfigManager;

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
                        DatabaseManager &db,
                        std::shared_ptr<UnifiedObservableConfigManager> config_manager);

        static bool Run(const std::vector<std::uint8_t> &image_data,
                        const std::string &original_file_path,
                        const BalancedPipelineConfig &cfg,
                        DatabaseManager &db,
                        std::shared_ptr<UnifiedObservableConfigManager> config_manager);
    };
}
