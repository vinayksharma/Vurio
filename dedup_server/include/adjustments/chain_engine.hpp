#pragma once

// CHAIN-1 (docs/todo_tool_chain.md §5): the one place pixels are produced
// for an edited image. Decoding stays in the existing paths (OpenCV for
// JPEG/PNG, the magick subprocess and LibRaw for everything else); the
// engine takes decoded pixels, brings them to the working size, runs the
// plan and encodes. Thumbnails, the viewer's full-size render, every live
// preview frame and (CHAIN-6) exports go through here.

#include "adjustments/frame.hpp"
#include "adjustments/render_plan.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace MediaDedup::Adjustments
{
    namespace ChainEngine
    {
        // 8-bit BGR (as cv::imread/imdecode produce) → float frame.
        Frame fromBgr8(const cv::Mat &bgr8);
        // Float frame → 8-bit BGR (rounded, clamped).
        cv::Mat toBgr8(const Frame &frame);

        // Decode a file the way the viewer sees it: EXIF orientation applied,
        // colour, optional reduced decode when the base is far larger than
        // `max_side` (libjpeg scaled decode). Empty on failure. Only for
        // formats OpenCV reads reliably (JPEG, PNG; TIFF without EXIF
        // orientation); the callers route other formats through magick.
        cv::Mat decodeBgr8(const std::string &path, int max_side = 0);
        cv::Mat decodeBgr8(const std::vector<uint8_t> &encoded);

        // Working size (§5.3): the scale that brings `base` to the smallest
        // size whose rendered output still reaches `target` on its longest
        // side. 1.0 when target <= 0 (native) or the base is small already.
        double workingScale(const RenderPlan &plan, Size base, int target);

        // Resize a frame by `scale` (INTER_AREA down, INTER_LINEAR up); a
        // scale of 1 returns the frame untouched.
        void scaleFrame(Frame &frame, double scale);

        // Fit a frame so its longest side is <= max_side (never enlarges).
        void fitFrame(Frame &frame, int max_side);

        // Gaussian blur of a float frame. Wide radii run on a downsampled
        // pyramid level and are upsampled back (docs §5.4: the one outlier
        // in the per-node budget), which is visually identical for soft
        // radii and bounds the cost.
        cv::Mat gaussianBlur(const cv::Mat &pixels, double sigma_px);

        // JPEG-encode a frame. Empty vector on failure.
        std::vector<uint8_t> encodeJpeg(const Frame &frame, int quality);

        // One-shot: decoded base → plan at the working size for `target` →
        // JPEG. `target` <= 0 renders at the base's own resolution. Returns
        // false (and logs) when the base is empty or encoding fails.
        bool renderToJpeg(const cv::Mat &base_bgr8, const RenderPlan &plan, int target, int quality,
                          std::vector<uint8_t> &jpeg_out, int *out_width = nullptr, int *out_height = nullptr,
                          const RenderPlan::RenderOptions &options = RenderPlan::RenderOptions());
    }
}
