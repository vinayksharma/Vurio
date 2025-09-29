#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <memory>

namespace MediaDedup
{
    class DatabaseManager;
    class UnifiedObservableConfigManager;

    struct FastPipelineConfig
    {
        int thumb_size = 256;
    };

    class FastPipeline
    {
    public:
        static bool Run(const std::string &file_path,
                        const FastPipelineConfig &cfg,
                        DatabaseManager &db,
                        std::shared_ptr<UnifiedObservableConfigManager> config_manager);

        static bool Run(const std::vector<std::uint8_t> &image_data,
                        const std::string &original_file_path,
                        const FastPipelineConfig &cfg,
                        DatabaseManager &db,
                        std::shared_ptr<UnifiedObservableConfigManager> config_manager);
    };
}
