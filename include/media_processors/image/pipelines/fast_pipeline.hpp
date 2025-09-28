#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace MediaDedup
{
    class DatabaseManager;

    struct FastPipelineConfig
    {
        int thumb_size = 256;
    };

    class FastPipeline
    {
    public:
        static bool Run(const std::string &file_path,
                        const FastPipelineConfig &cfg,
                        DatabaseManager &db);

        static bool Run(const std::vector<std::uint8_t> &image_data,
                        const std::string &original_file_path,
                        const FastPipelineConfig &cfg,
                        DatabaseManager &db);
    };
}
