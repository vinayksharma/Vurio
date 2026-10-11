#pragma once

// ADJ-1 / CHAIN-1: Rotate — exact quarter turns plus a fine straighten
// angle. Geometry: the canvas grows to the rotated bounding box (the
// ImageMagick canvas rule in adjustment_geometry.hpp, kept so the client's
// geometry and every stored crop stay valid).
// Params: {"version":1,"quarter_turns":0..3,"angle":-45..45}

#include "adjustments/adjustment_tool.hpp"

namespace MediaDedup::Adjustments
{
    struct RotateState final : ToolState
    {
        int quarter_turns = 0;  // clockwise quarter turns, 0..3
        double angle = 0.0;     // degrees clockwise, [-45, 45]
        std::unique_ptr<ToolState> clone() const override { return std::make_unique<RotateState>(*this); }
    };

    class RotateTool final : public AdjustmentTool
    {
    public:
        static constexpr const char *kId = "rotate";
        static constexpr int kVersion = 1;
        static constexpr double kMaxAngle = 45.0;

        RotateTool();
        const ToolDescriptor &descriptor() const override { return descriptor_; }
        std::unique_ptr<ToolState> defaultState() const override;
        std::unique_ptr<ToolState> parse(const nlohmann::json &section, std::string &error) const override;
        nlohmann::json serialize(const ToolState &state) const override;
        bool isIdentity(const ToolState &state) const override;
        bool validate(const ToolState &state, Size input, std::string &error) const override;
        void apply(const ToolState &state, Frame &io, const RenderContext &ctx) const override;
        Size transformSize(const ToolState &state, Size input) const override;

        // Total rotation, degrees clockwise.
        static double totalDegrees(const RotateState &s) { return s.quarter_turns * 90.0 + s.angle; }

    private:
        ToolDescriptor descriptor_;
    };
}
