#pragma once

// CHAIN-1 (docs/todo_tool_chain.md §5.2): the currency of the engine. A
// frame is the image between two nodes: float pixels in gamma sRGB, [0,1],
// OpenCV's BGR channel order, plus the canvas size. Float keeps a long chain
// free of banding; the conversion from the 8-bit decode is paid once per
// base load.

#include "adjustments/adjustment_geometry.hpp"
#include <opencv2/core.hpp>

namespace MediaDedup::Adjustments
{
    struct Frame
    {
        cv::Mat pixels; // CV_32FC3, BGR, [0,1]

        bool empty() const { return pixels.empty(); }
        int width() const { return pixels.cols; }
        int height() const { return pixels.rows; }
        Size size() const { return {static_cast<double>(pixels.cols), static_cast<double>(pixels.rows)}; }
        // Pixels per normalised unit: a tool's "radius 0.01" is 1 % of the
        // shorter side of the canvas it receives (§3.4).
        double pixelScale() const { return static_cast<double>(std::min(pixels.cols, pixels.rows)); }
        size_t bytes() const { return pixels.empty() ? 0 : pixels.total() * pixels.elemSize(); }
    };

    // What a tool receives beside the frame.
    struct RenderContext
    {
        double pixel_scale = 1.0; // = frame.pixelScale() of the node's input
    };
}
