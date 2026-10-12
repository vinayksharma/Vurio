#pragma once

// CHAIN-7a (docs/todo_tool_chain.md §12): the texture tools of the second
// wave — Grain (layer), Clarity and Dehaze (raster). ParamTools; formulas in
// the class comments, pinned by test_layer_tools.cpp. Lengths are fractions
// of the frame's shorter side (§3.4), so the result is the same at every
// working size.

#include "adjustments/tools/param_tool.hpp"

namespace MediaDedup::Adjustments
{
    // Seeded film grain: Gaussian noise n drawn by cv::RNG(seed) on a grid
    // of `size`-sized cells (so every working size sees the same pattern),
    // upsampled bicubic. out = in + amount·0.2·n·(1 − 0.5·L), where n mixes
    // the monochrome field with per-channel fields by `color`. Identity
    // when amount is 0, whatever the seed.
    class GrainTool final : public ParamTool
    {
    public:
        static constexpr const char *kId = "grain";
        GrainTool();
        bool isIdentity(const ToolState &state) const override;
        void apply(const ToolState &, Frame &io, const RenderContext &) const override;
        // The noise fields for a frame size (CV_32FC1 mono, CV_32FC3 colour).
        static void fields(int width, int height, double cell_px, int seed, cv::Mat &mono, cv::Mat &colour);
    };

    // Local contrast on luma: d = L − blur(L, radius), w = 4L(1 − L),
    // out = in + amount·2·d·w. Negative amount softens.
    class ClarityTool final : public ParamTool
    {
    public:
        static constexpr const char *kId = "clarity";
        ClarityTool();
        void apply(const ToolState &, Frame &io, const RenderContext &) const override;
    };

    // Dark-channel prior: dark = min-filter(min_c in_c) over a window of
    // 1.5 % of the short side; A = mean colour of the brightest 0.1 % of
    // dark-channel pixels; t = clamp(1 − 0.95·amount·dark/max_c A_c, 0.1, 1)
    // refined by a guided filter against luma (4 % of the short side);
    // out = (in − A)/t + A. Negative amount adds haze toward A:
    // out = in + (A − in)·(−amount)/2.
    class DehazeTool final : public ParamTool
    {
    public:
        static constexpr const char *kId = "dehaze";
        DehazeTool();
        void apply(const ToolState &, Frame &io, const RenderContext &) const override;
        // Atmospheric light estimate (BGR) for a frame.
        static cv::Vec3f atmosphericLight(const cv::Mat &pixels, const cv::Mat &dark);
    };
}
