#pragma once

// CHAIN-7f (docs/todo_tool_chain.md §12): keystone correction. A geometry
// tool (trunk only, like Rotate and Crop): `vertical` and `horizontal` in
// [−1, 1] pull one edge's corners inward by up to 25 % of the canvas
// (vertical > 0 narrows the top, < 0 the bottom; horizontal > 0 narrows the
// right edge, < 0 the left), which is the warp that squares converging
// lines. With `auto_fill` (default) the largest canvas-shaped rectangle
// inside the warped content is scaled up to the canvas, so no empty corners
// appear; without it the canvas keeps its size and the corners go black for
// a crop to remove. The canvas size never changes. The homography composes
// onto the frame's base→canvas transform, so masks follow the picture.

#include "adjustments/tools/param_tool.hpp"

namespace MediaDedup::Adjustments
{
    class PerspectiveTool final : public ParamTool
    {
    public:
        static constexpr const char *kId = "perspective";
        static constexpr double kPull = 0.25;
        PerspectiveTool();
        void apply(const ToolState &, Frame &io, const RenderContext &) const override;

        // The keystone homography for a canvas, before any fill.
        static cv::Matx33d keystone(double width, double height, double vertical, double horizontal);
        // The scale s of the largest centred rectangle of the canvas's
        // aspect that lies inside the warped canvas (0 < s <= 1).
        static double fillScale(double width, double height, const cv::Matx33d &h);
    };
}
