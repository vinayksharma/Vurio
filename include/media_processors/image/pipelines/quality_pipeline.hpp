#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <memory>

namespace MediaDedup
{
    class DatabaseManager;
    class UnifiedObservableConfigManager;

    struct QualityPipelineConfig
    {
        int input_size = 224;
        int embedding_dim = 512;
        std::string model = "CLIP-RN50";
    };

    class QualityPipeline
    {
    public:
        static bool Run(const std::string &file_path,
                        const QualityPipelineConfig &cfg,
                        DatabaseManager &db,
                        std::shared_ptr<UnifiedObservableConfigManager> config_manager);

        static bool Run(const std::vector<std::uint8_t> &image_data,
                        const std::string &original_file_path,
                        const QualityPipelineConfig &cfg,
                        DatabaseManager &db,
                        std::shared_ptr<UnifiedObservableConfigManager> config_manager);
    };
}
