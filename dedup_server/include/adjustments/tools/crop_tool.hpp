#pragma once

// ADJ-1 / CHAIN-1: Crop — a rectangle normalized to the canvas the node
// receives, plus the UI's aspect lock (stored so the panel restores it; it
// does not affect the render).
// Params: {"version":1,"left","top","width","height" in [0,1],
//           "aspect": free|original|1:1|3:2|4:3|5:4|16:9|custom,
//           "custom_aspect": w/h when aspect == custom}

#include "adjustments/adjustment_tool.hpp"

namespace MediaDedup::Adjustments
{
    struct CropState final : ToolState
    {
        NormRect rect;                 // normalized on the rotated canvas
        std::string aspect = "free";   // one of CropTool::kAspectOptions
        double custom_aspect = 1.0;    // width / height, used when aspect == "custom"
        std::unique_ptr<ToolState> clone() const override { return std::make_unique<CropState>(*this); }
    };

    class CropTool final : public AdjustmentTool
    {
    public:
        static constexpr const char *kId = "crop";
        static constexpr int kVersion = 1;
        static constexpr double kMinExtent = 0.01;   // a crop smaller than 1% is a mistake
        static constexpr double kMinCustomAspect = 0.1;
        static constexpr double kMaxCustomAspect = 10.0;
        static const std::vector<std::string> kAspectOptions;

        CropTool();
        const ToolDescriptor &descriptor() const override { return descriptor_; }
        std::unique_ptr<ToolState> defaultState() const override;
        std::unique_ptr<ToolState> parse(const nlohmann::json &section, std::string &error) const override;
        nlohmann::json serialize(const ToolState &state) const override;
        bool isIdentity(const ToolState &state) const override;
        // `input` is the canvas the node receives; the plan validates the
        // crop-inside-content rule, which needs the rotation too (see
        // RenderPlan::validate).
        bool validate(const ToolState &state, Size input, std::string &error) const override;
        void apply(const ToolState &state, Frame &io, const RenderContext &ctx) const override;
        // Pixel rectangle of the crop on a canvas: the same rounding the
        // engine cuts with and displaySize() predicts.
        static cv::Rect pixelRect(const NormRect &rect, int width, int height);
        Size transformSize(const ToolState &state, Size input) const override;

    private:
        ToolDescriptor descriptor_;
    };
}
