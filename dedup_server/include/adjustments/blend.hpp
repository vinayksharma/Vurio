#pragma once

// CHAIN-2 (docs/todo_tool_chain.md §5.3): the sixteen blend modes a layer
// node may carry, computed by the engine on gamma sRGB in [0,1]. The
// separable modes follow the W3C compositing formulas (which are
// Photoshop's); the four non-separable ones (hue, saturation, color,
// luminosity) use the W3C SetLum/SetSat construction.

#include "adjustments/frame.hpp"
#include <string>
#include <vector>

namespace MediaDedup::Adjustments::Blend
{
    const std::vector<std::string> &modes();
    bool isMode(const std::string &name);

    // out = blend(base, top); out may alias base.
    void apply(const std::string &mode, const cv::Mat &base, const cv::Mat &top, cv::Mat &out);

    // One pixel, for tests and the per-pixel loops: b, t in [0,1] BGR.
    cv::Vec3f blendPixel(const std::string &mode, const cv::Vec3f &b, const cv::Vec3f &t);
}
