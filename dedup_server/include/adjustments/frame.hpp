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
        // CHAIN-7f: a 3×3 homography, so a perspective node composes like
        // the affine ones; rotate, crop and the resize keep composing 2×3
        // transforms, which are lifted. `isAffine()` tells the mask code
        // when the cheaper affine paths apply.
        cv::Matx33d base_to_canvas = cv::Matx33d::eye();

        // Start the transform for a base of this pixel size.
        void resetAffine()
        {
            base_to_canvas = cv::Matx33d(static_cast<double>(pixels.cols), 0, 0, 0, static_cast<double>(pixels.rows), 0, 0, 0, 1);
        }
        // Compose a 2x3 transform applied to the pixels: new = m ∘ old.
        void composeAffine(const cv::Matx23d &m)
        {
            cv::Matx33d b(m(0, 0), m(0, 1), m(0, 2), m(1, 0), m(1, 1), m(1, 2), 0, 0, 1);
            base_to_canvas = b * base_to_canvas;
        }
        // Compose a 3x3 projective transform applied to the pixels.
        void composeHomography(const cv::Matx33d &h) { base_to_canvas = h * base_to_canvas; }

        bool isAffine() const
        {
            return std::fabs(base_to_canvas(2, 0)) < 1e-12 && std::fabs(base_to_canvas(2, 1)) < 1e-12 &&
                   std::fabs(base_to_canvas(2, 2) - 1.0) < 1e-12;
        }

        // A normalised base point in frame pixels.
        cv::Point2d project(double nx, double ny) const
        {
            const cv::Matx33d &m = base_to_canvas;
            double w = m(2, 0) * nx + m(2, 1) * ny + m(2, 2);
            if (std::fabs(w) < 1e-12)
                w = 1e-12;
            return {(m(0, 0) * nx + m(0, 1) * ny + m(0, 2)) / w, (m(1, 0) * nx + m(1, 1) * ny + m(1, 2)) / w};
        }
        // Frame pixels → normalised base (the inverse mapping).
        cv::Point2d unproject(double px, double py) const
        {
            cv::Matx33d inv = base_to_canvas.inv();
            double w = inv(2, 0) * px + inv(2, 1) * py + inv(2, 2);
            if (std::fabs(w) < 1e-12)
                w = 1e-12;
            return {(inv(0, 0) * px + inv(0, 1) * py + inv(0, 2)) / w, (inv(1, 0) * px + inv(1, 1) * py + inv(1, 2)) / w};
        }
        // The Jacobian of `project` at a base point: the local 2×2 linear
        // map (exactly the affine part for an affine transform).
        cv::Matx22d jacobian(double nx, double ny) const
        {
            const cv::Matx33d &m = base_to_canvas;
            double w = m(2, 0) * nx + m(2, 1) * ny + m(2, 2);
            if (std::fabs(w) < 1e-12)
                w = 1e-12;
            double u = m(0, 0) * nx + m(0, 1) * ny + m(0, 2);
            double v = m(1, 0) * nx + m(1, 1) * ny + m(1, 2);
            double w2 = w * w;
            return cv::Matx22d((m(0, 0) * w - u * m(2, 0)) / w2, (m(0, 1) * w - u * m(2, 1)) / w2,
                               (m(1, 0) * w - v * m(2, 0)) / w2, (m(1, 1) * w - v * m(2, 1)) / w2);
        }
        // Pixels per normalised unit of the base's shorter side, after the
        // geometry above (the length scale for mask radii and feathers).
        // Measured at the base's centre for a projective transform.
        double affineScale() const
        {
            cv::Matx22d j = jacobian(0.5, 0.5);
            double sx = std::hypot(j(0, 0), j(1, 0));
            double sy = std::hypot(j(0, 1), j(1, 1));
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
