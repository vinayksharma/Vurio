#pragma once

// LOOK-1/2 (docs/todo_film_looks.md): the tools a film look is assembled
// from. None of them is a look in itself: Blur and Luma Key build glows
// (halation, bloom, mist) inside a lane, Channel Mixer and Colour Grade
// shape a colour response. Gamma sRGB in [0,1], BGR, like the layer tools.

#include "adjustments/tools/param_tool.hpp"

namespace MediaDedup::Adjustments
{
    // Gaussian of sigma = radius × short side. With `linear` the blur is
    // made in linear light (decode, blur, encode), so a bright source
    // spreads as light does instead of going grey at its rim.
    class BlurTool final : public ParamTool
    {
    public:
        static constexpr const char *kId = "blur";
        BlurTool();
        void apply(const ToolState &, Frame &io, const RenderContext &) const override;
    };

    // out = in × w. k is the pixel's brightest channel or its luma;
    // w = smoothstep(threshold − softness, threshold + softness, k) to keep
    // the highlights, 1 − that to keep the shadows. What is not kept is
    // black, ready to be blurred and added back.
    class LumaKeyTool final : public ParamTool
    {
    public:
        static constexpr const char *kId = "luma_key";
        LumaKeyTool();
        void apply(const ToolState &, Frame &io, const RenderContext &) const override;
        // The defaults already key (active_at_defaults).
        bool isIdentity(const ToolState &) const override { return false; }
    };

    // out_c = c_r·R + c_g·G + c_b·B for c in {red, green, blue}.
    class ChannelMixerTool final : public ParamTool
    {
    public:
        static constexpr const char *kId = "channel_mixer";
        ChannelMixerTool();
        void apply(const ToolState &, Frame &io, const RenderContext &) const override;
    };

    // L' = L^(2^−balance); weights (1−L')², 2L'(1−L'), L'² for shadows,
    // midtones, highlights (they sum to 1). Each range adds
    // w·(amount·0.5·(C − luma(C)) + lum·0.25), C = HSV(hue, 1, 1): a tint
    // that leaves luma alone, and a lift or drop.
    class ColorGradeTool final : public ParamTool
    {
    public:
        static constexpr const char *kId = "color_grade";
        ColorGradeTool();
        void apply(const ToolState &, Frame &io, const RenderContext &) const override;
    };
}
