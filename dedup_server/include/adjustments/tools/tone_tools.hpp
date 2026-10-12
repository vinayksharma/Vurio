#pragma once

// CHAIN-7a (docs/todo_tool_chain.md §12): the tone and colour tools of the
// second wave — Levels, Curves and HSL. ParamTools like the first wave
// (layer_tools.hpp): the formula in the class comment, pinned by
// test_layer_tools.cpp; the engine composes blend and strength. Maths on
// gamma sRGB in [0,1], frames BGR, L = BT.709 luma.

#include "adjustments/tools/param_tool.hpp"
#include <vector>

namespace MediaDedup::Adjustments
{
    // t = clamp((in − in_black) / (in_white − in_black))^(1/γ),
    // out = out_black + t·(out_white − out_black). One LUT for all channels.
    class LevelsTool final : public ParamTool
    {
    public:
        static constexpr const char *kId = "levels";
        LevelsTool();
        bool validate(const ToolState &state, Size input, std::string &error) const override;
        void apply(const ToolState &, Frame &io, const RenderContext &) const override;
        static double map(double in, double ib, double iw, double gamma, double ob, double ow);
    };

    // Four curves: out_c = C_c(RGB(in_c)) — the master curve first, then the
    // channel's own. Each curve is a monotone cubic through its points
    // (CurvePoints); evaluated through a 4096-entry LUT per channel.
    class CurvesTool final : public ParamTool
    {
    public:
        static constexpr const char *kId = "curves";
        static constexpr int kLutSize = 4096;
        CurvesTool();
        void apply(const ToolState &, Frame &io, const RenderContext &) const override;
    };

    // Eight hue bands (red 0°, orange 30°, yellow 60°, green 120°, aqua
    // 180°, blue 240°, purple 280°, magenta 320°), each with a hue shift
    // (degrees), a saturation change and a luminance change. For a pixel
    // of hue h the band weights are the piecewise-linear partition of unity
    // between the two nearest centres (cyclic); the summed adjustment is
    // scaled by f = smoothstep(0, 0.25, s) so greys are untouched. In HSV:
    //   h' = h + f·Δh,  s' = s·(1 + f·Δs),
    //   v' = Δl ≥ 0 ? v + (1 − v)·f·Δl/2 : v·(1 + f·Δl/2).
    class HslTool final : public ParamTool
    {
    public:
        static constexpr const char *kId = "hsl";
        static constexpr int kBands = 8;
        static const char *const kBandIds[kBands];
        static const char *const kBandLabels[kBands];
        static const double kBandCentres[kBands];
        HslTool();
        void apply(const ToolState &, Frame &io, const RenderContext &) const override;
        // The band weights for a hue in degrees (sum to 1).
        static void weights(double hue_degrees, double out[kBands]);
    };

    // Apply per-channel LUTs (B, G, R; each over [0,1]) to a frame with
    // linear interpolation between entries. Shared by Levels and Curves.
    void applyLuts(Frame &io, const std::vector<float> &lut_b, const std::vector<float> &lut_g,
                   const std::vector<float> &lut_r);
}
