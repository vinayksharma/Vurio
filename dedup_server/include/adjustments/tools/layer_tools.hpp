#pragma once

// CHAIN-2 (docs/todo_tool_chain.md §6): the layer and raster tools. Each is
// a ParamTool with its formula in the class comment; the engine composes the
// result with the node's blend and strength (RenderPlan), so a tool only
// produces its derived frame. All maths on gamma sRGB in [0,1]; L is the
// BT.709 luma 0.2126 R + 0.7152 G + 0.0722 B. Frames are BGR.

#include "adjustments/tools/param_tool.hpp"

namespace MediaDedup::Adjustments
{
    // out = in × 2^ev
    class ExposureTool final : public ParamTool
    {
    public:
        static constexpr const char *kId = "exposure";
        ExposureTool();
        void apply(const ToolState &, Frame &io, const RenderContext &) const override;
    };

    // out = (in − 0.5)·c + 0.5 + brightness, c = 1 + contrast
    class BrightContrastTool final : public ParamTool
    {
    public:
        static constexpr const char *kId = "bright_contrast";
        BrightContrastTool();
        void apply(const ToolState &, Frame &io, const RenderContext &) const override;
    };

    // Channel gains R·2^(k·t), G·2^(−m·tint), B·2^(−k·t), scaled so the
    // luma of mid-grey is unchanged. Multiplicative, so the response is
    // symmetric, no gain can reach zero, and ±1 spans a 16:1 red/blue ratio
    // (k = 2) and a 2:1 green one (m = 1) — enough to neutralise tungsten
    // light shot on a daylight balance.
    //
    // Params version 1 used linear gains R·(1+0.3t), G·(1−0.2·tint),
    // B·(1−0.3t), which stopped at a 1.86:1 red/blue ratio. `upgradeParams`
    // converts a version-1 pair to the version-2 pair with the same gains.
    class WhiteBalanceTool final : public ParamTool
    {
    public:
        static constexpr const char *kId = "white_balance";
        static constexpr double kTemperatureStops = 2.0; // k
        static constexpr double kTintStops = 1.0;        // m
        WhiteBalanceTool();
        bool upgradeParams(nlohmann::json &params) const override;
        void apply(const ToolState &, Frame &io, const RenderContext &) const override;
        static void gains(double temperature, double tint, double &gr, double &gg, double &gb);
        // The temperature and tint that make `input` neutral: R = B gives
        // the temperature and G = their mean gives the tint. A cast beyond
        // the sliders' reach is corrected as far as they go.
        bool pick(const PickedColour &input, std::vector<std::pair<std::string, double>> &values,
                  std::string &note, std::string &error) const override;
    };

    // out = L + (in − L)·s, s = 1 + amount
    class SaturationTool final : public ParamTool
    {
    public:
        static constexpr const char *kId = "saturation";
        SaturationTool();
        void apply(const ToolState &, Frame &io, const RenderContext &) const override;
    };

    // YIQ rotation by `degrees`.
    class HueTool final : public ParamTool
    {
    public:
        static constexpr const char *kId = "hue";
        HueTool();
        void apply(const ToolState &, Frame &io, const RenderContext &) const override;
    };

    // g = r·R + g·G + b·B (preset weights or custom), then an optional tint
    // toward a hue: out = g·(1 − a) + g ⊗ C·a, C = HSV(hue, 0.35, 1).
    class MonochromeTool final : public ParamTool
    {
    public:
        static constexpr const char *kId = "monochrome";
        MonochromeTool();
        void apply(const ToolState &, Frame &io, const RenderContext &) const override;
        /// Identity only when the conversion is switched off: the defaults
        /// already convert (active_at_defaults).
        bool isIdentity(const ToolState &state) const override;
        static void weights(const std::string &preset, double r, double g, double b, double &wr, double &wg, double &wb);
        static void presetWeights(const std::string &preset, double &wr, double &wg, double &wb);
    };

    // out = in·(1 − d) + (in ⊗ C·k)·d, C = HSV(hue, saturation, 1), k scales
    // C so its luma is 1 (luminance preserving; channels clamp at 1).
    class ColorFilterTool final : public ParamTool
    {
    public:
        static constexpr const char *kId = "color_filter";
        ColorFilterTool();
        void apply(const ToolState &, Frame &io, const RenderContext &) const override;
    };

    // out = in^(1/γ)
    class GammaTool final : public ParamTool
    {
    public:
        static constexpr const char *kId = "gamma";
        GammaTool();
        void apply(const ToolState &, Frame &io, const RenderContext &) const override;
    };

    // out = in × (1 − amount·f(r)); r is the normalised radius from the
    // centre (an ellipse fitting the frame, rounded toward a circle by
    // `roundness`), f = smoothstep(midpoint, midpoint + feather, r).
    class VignetteTool final : public ParamTool
    {
    public:
        static constexpr const char *kId = "vignette";
        VignetteTool();
        void apply(const ToolState &, Frame &io, const RenderContext &) const override;
        static float falloff(double nx, double ny, double w, double h, double midpoint, double roundness, double feather);
    };

    // Unsharp mask: out = in + amount·(in − blur(in, radius)), where the
    // difference is applied only beyond `threshold`. `radius` is a fraction
    // of the shorter side (resolution independent).
    class SharpenTool final : public ParamTool
    {
    public:
        static constexpr const char *kId = "sharpen";
        SharpenTool();
        void apply(const ToolState &, Frame &io, const RenderContext &) const override;
    };

    // Non-local-means denoise (OpenCV fastNlMeansDenoisingColored) with
    // strengths h = 3 + 12·luminance, hColor = 3 + 12·color, on 8-bit pixels.
    class DenoiseTool final : public ParamTool
    {
    public:
        static constexpr const char *kId = "denoise";
        DenoiseTool();
        void apply(const ToolState &, Frame &io, const RenderContext &) const override;
    };

    // HSV (h in degrees, s and v in [0,1]) → BGR in [0,1].
    cv::Vec3f hsvToBgr(double h, double s, double v);
}
