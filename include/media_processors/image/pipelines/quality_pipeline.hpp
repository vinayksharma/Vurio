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
        static bool Run(const std::string &file_path,
                        const QualityPipelineConfig &cfg,
                        DatabaseManager &db);

        static bool Run(const std::vector<std::uint8_t> &image_data,
                        const std::string &original_file_path,
                        const QualityPipelineConfig &cfg,
                        DatabaseManager &db);
    };
}
