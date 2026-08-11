#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <memory>

namespace MediaDedup
{
    class UnifiedObservableConfigManager;
    class OnnxSessionManager;

    class OnnxAdapter
    {
    public:
        // Initialize the global session manager (call once at startup)
        static void initializeSessionManager(std::shared_ptr<UnifiedObservableConfigManager> config);
        
        // Shutdown the global session manager (call at cleanup)
        static void shutdownSessionManager();
        
        // Returns false if ONNX Runtime is not available or model cannot be loaded.
        // If out_reason is non-null and the call fails, it is set to a brief diagnostic string.
        static bool ComputeEmbedding(const std::string &file_path,
                                     int input_size,
                                     const std::string &model_path,
                                     int embedding_dim,
                                     std::vector<std::uint8_t> &out_blob,
                                     std::string *out_reason = nullptr);

        // Memory-based version for processing image data directly from memory
        static bool ComputeEmbedding(const std::vector<std::uint8_t> &image_data,
                                     int input_size,
                                     const std::string &model_path,
                                     int embedding_dim,
                                     std::vector<std::uint8_t> &out_blob);

        // Run the CLIP TEXT encoder on pre-tokenized input (see ClipTokenizer).
        // out_floats receives embedding_dim floats, NOT normalized — callers
        // L2-normalize to match the image embeddings stored via
        // ImageArtifactsOps. Returns false if ONNX Runtime is unavailable or
        // the model cannot be loaded/run.
        static bool ComputeTextEmbedding(const std::vector<std::int64_t> &input_ids,
                                         const std::vector<std::int64_t> &attention_mask,
                                         const std::string &model_path,
                                         int embedding_dim,
                                         std::vector<float> &out_floats,
                                         std::string *out_reason = nullptr);

        // DF-4 test hook: decode exactly as ComputeEmbedding does (reduced
        // decode with full-resolution fallback) and report the decoded
        // dimensions. Returns false when the file cannot be decoded or
        // OpenCV is unavailable.
        static bool DecodeDimsForTest(const std::string &file_path,
                                      int input_size,
                                      int &out_rows,
                                      int &out_cols);

    private:
        static std::shared_ptr<OnnxSessionManager> session_manager_;
    };
}
