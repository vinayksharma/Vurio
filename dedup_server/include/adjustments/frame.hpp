#pragma once

// CHAIN-1 (docs/todo_tool_chain.md §5.2): the currency of the engine. A
// frame is the image between two nodes: float pixels in gamma sRGB, [0,1],
// OpenCV's BGR channel order, plus the canvas size. Float keeps a long chain
// free of banding; the conversion from the 8-bit decode is paid once per
// base load.

#include "adjustments/adjustment_geometry.hpp"
#include <cmath>
#include <opencv2/core.hpp>

namespace MediaDedup::Adjustments
{
    struct Frame
    {
        cv::Mat pixels; // CV_32FC3, BGR, [0,1]
        // CHAIN-4 (docs §3.4): normalised BASE coordinates (0..1 of the
        // EXIF-oriented base) → pixels of this frame. Geometry nodes and
        // the working-size resize compose onto it, so a mask stored in base
        // space lands on the same part of the picture whatever is above it.
        cv::Matx23d base_to_canvas = cv::Matx23d(1, 0, 0, 0, 1, 0);

        // Start the affine for a base of this pixel size.
        void resetAffine()
        {
            base_to_canvas = cv::Matx23d(static_cast<double>(pixels.cols), 0, 0, 0, static_cast<double>(pixels.rows), 0);
        }
        // Compose a 2x3 transform applied to the pixels: new = m ∘ old.
        void composeAffine(const cv::Matx23d &m)
        {
            cv::Matx33d a(base_to_canvas(0, 0), base_to_canvas(0, 1), base_to_canvas(0, 2),
                          base_to_canvas(1, 0), base_to_canvas(1, 1), base_to_canvas(1, 2), 0, 0, 1);
            cv::Matx33d b(m(0, 0), m(0, 1), m(0, 2), m(1, 0), m(1, 1), m(1, 2), 0, 0, 1);
            cv::Matx33d c = b * a;
            base_to_canvas = cv::Matx23d(c(0, 0), c(0, 1), c(0, 2), c(1, 0), c(1, 1), c(1, 2));
        }
        // A normalised base point in frame pixels.
        cv::Point2d project(double nx, double ny) const
        {
            return {base_to_canvas(0, 0) * nx + base_to_canvas(0, 1) * ny + base_to_canvas(0, 2),
                    base_to_canvas(1, 0) * nx + base_to_canvas(1, 1) * ny + base_to_canvas(1, 2)};
        }
        // Pixels per normalised unit of the base's shorter side, after the
        // geometry above (the length scale for mask radii and feathers).
        double affineScale() const
        {
            double sx = std::hypot(base_to_canvas(0, 0), base_to_canvas(1, 0));
            double sy = std::hypot(base_to_canvas(0, 1), base_to_canvas(1, 1));
            return std::min(sx, sy);
        }

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
