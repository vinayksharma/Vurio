#pragma once

// CHAIN-2 (docs/todo_tool_chain.md §5.3): the blend modes a layer
// node may carry, computed by the engine on gamma sRGB in [0,1]. The
// separable modes follow the W3C compositing formulas (which are
// Photoshop's); the four non-separable ones (hue, saturation, color,
// luminosity) use the W3C SetLum/SetSat construction. LOOK-1 adds `add`
// (b + t, Photoshop's Linear Dodge) and `linear_add` (the same sum made in
// linear light, which is how light itself adds).

#include "adjustments/frame.hpp"
#include <string>
#include <vector>

namespace MediaDedup::Adjustments::Blend
{
    const std::vector<std::string> &modes();
    bool isMode(const std::string &name);

    // out = blend(base, top); out may alias base.
    void apply(const std::string &mode, const cv::Mat &base, const cv::Mat &top, cv::Mat &out);

    // LOOK-1: gamma sRGB <-> linear light, per value, through a table.
    // Values are clamped to [0,1] first.
    float toLinear(float gamma);
    float toGamma(float linear);
    void toLinear(cv::Mat &pixels); // any CV_32F channel count, in place
    void toGamma(cv::Mat &pixels);

    // One pixel, for tests and the per-pixel loops: b, t in [0,1] BGR.
    cv::Vec3f blendPixel(const std::string &mode, const cv::Vec3f &b, const cv::Vec3f &t);
}
