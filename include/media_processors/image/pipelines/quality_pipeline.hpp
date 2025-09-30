#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace MediaDedup
{
    class DatabaseManager;

    struct QualityPipelineConfig
    {
        int input_size = 224;
        int embedding_dim = 512;
        std::string model = "CLIP-RN50";
    };

    class QualityPipeline
    {
    public:
        // Load config from unified config manager
        // Falls back to provided defaults when properties are missing
        static QualityPipelineConfig GetConfigFromManager(std::shared_ptr<class UnifiedObservableConfigManager> config_manager,
                                                          const QualityPipelineConfig &defaults = {});

        static bool Run(const std::string &file_path,
                        const QualityPipelineConfig &cfg,
                        DatabaseManager &db);

        static bool Run(const std::vector<std::uint8_t> &image_data,
                        const std::string &original_file_path,
                        const QualityPipelineConfig &cfg,
                        DatabaseManager &db);
    };
}
