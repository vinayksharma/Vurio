#pragma once

// CHAIN-7h (docs/todo_tool_chain.md §12): the two restoration networks an
// export may run once over the rendered frame — AI denoise and ×2 upscale.
// They are export-time operations, not chain nodes: a convolutional
// denoise on a 2560 px preview takes seconds on a CPU and an upscale of a
// 24 MP base takes a minute even on the Apple GPU, so neither belongs in
// the per-drag render budget (§5.4). Both are tiled (512 px, 32 px
// overlap) so any size fits in memory; both are portable ONNX models
// fetched by ensure_models.sh and optional: a missing file hides the
// control in the export sheet.
//
//   denoise: NAFNet-SIDD-width32 (MIT), NCHW RGB [0,1] in → out. Measured
//            0.54 s per 512² tile on 4 CPU threads; the CoreML provider
//            partitions it badly (2.4 s), so it always runs on the CPU.
//   upscale: Real-ESRGAN x2plus (BSD-3), NCHW RGB [0,1] in → 2× out.
//            6.9 s per tile on the CPU, 0.6 s through CoreML (identical
//            output, PSNR 89 dB), so CoreML is tried first.

#include <functional>
#include <opencv2/core.hpp>
#include <string>

namespace MediaDedup::Adjustments::ExportModels
{
    struct Models
    {
        std::string denoise_model;
        std::string upscale_model;
        bool upscale_coreml = true;
    };
    void configure(const Models &models);
    void unload();

    struct Status
    {
        bool available = false;
        bool model_present = false;
        bool loaded = false;
        std::string model_path;
        std::string note;
    };
    Status denoiseStatus();
    Status upscaleStatus();
    bool denoiseAvailable();
    bool upscaleAvailable();

    constexpr int kUpscaleFactor = 2;
    constexpr int kTile = 512;
    constexpr int kOverlap = 32;

    using Progress = std::function<void(int done, int total)>;

    // In place on a CV_32FC3 BGR frame in [0,1]. `strength` mixes the
    // denoised result back (1 = fully denoised). False with `error` when
    // the model cannot run; the frame is then untouched.
    bool denoise(cv::Mat &bgr32, double strength, std::string *error = nullptr, const Progress &progress = nullptr);
    // Replaces the frame with its ×2 upscale.
    bool upscale(cv::Mat &bgr32, std::string *error = nullptr, const Progress &progress = nullptr);
}
