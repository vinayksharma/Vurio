#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace MediaDedup
{
    class OnnxAdapter
    {
    public:
        // Returns false if ONNX Runtime is not available or model cannot be loaded
        static bool ComputeEmbedding(const std::string &file_path,
                                     int input_size,
                                     const std::string &model_path,
                                     int embedding_dim,
                                     std::vector<std::uint8_t> &out_blob);
    };
}



