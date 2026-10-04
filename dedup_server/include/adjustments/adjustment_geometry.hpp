#pragma once

// ADJ-1 (docs/todo_image_adjustments.md §3.3): the geometry every adjustment
// tool and both renderers (ImageMagick on the server, the Swift viewer on
// the client) agree on. Pure functions, header-only, no dependencies, so the
// list-query code and the tools can share it without linking anything.
//
// Coordinate contract:
//   - Angles are degrees, positive = clockwise (ImageMagick -rotate and
//     SwiftUI rotationEffect both read them that way).
//   - A rotation is `quarter_turns` (0..3) exact quarter turns followed by a
//     fine `angle` in [-45, 45]. The canvas after rotation is the bounding
//     box of the rotated base image, which is what ImageMagick's -rotate
//     produces (it decomposes any angle into integral turns + a residual).
//   - A crop is a rectangle NORMALIZED to that rotated canvas, origin top
//     left, so the same document renders identically from the original,
//     from a 2560 px preview, or from a RAW's embedded JPEG.
//
// ImageMagick's -rotate canvas is NOT the analytic bounding box: its
// three-shear algorithm yields ceil(analytic + 1) rounded up to an even
// number, on each axis (measured on 53 size/angle pairs, 2026-10-03, IM
// 7.1.2 — e.g. 1000x800 at 3 deg: analytic 1040.5x851.2, canvas 1042x854).
// rotatedBounds() reproduces that rule so display geometry matches the
// rendered pixels exactly; the Swift client implements the same rule. The
// content-fit tests (cropInsideContent, largestInscribedRect) work on the
// analytic rectangle centered in that canvas, which is where -rotate places
// the image (it pads symmetrically).

#include <algorithm>
#include <cmath>

namespace MediaDedup::Adjustments
{
    constexpr double kPi = 3.14159265358979323846;

    struct Size
    {
        double width = 0;
        double height = 0;
    };

    // Normalized rectangle in [0,1] canvas coordinates.
    struct NormRect
    {
        double left = 0;
        double top = 0;
        double width = 1;
        double height = 1;

        double right() const { return left + width; }
        double bottom() const { return top + height; }
        bool isFull() const
        {
            return std::fabs(left) < 1e-9 && std::fabs(top) < 1e-9 &&
                   std::fabs(width - 1.0) < 1e-9 && std::fabs(height - 1.0) < 1e-9;
        }
    };

    inline double degreesToRadians(double degrees) { return degrees * kPi / 180.0; }

    // Base size after the exact quarter turns (odd turns swap the axes).
    inline Size afterQuarterTurns(Size base, int quarter_turns)
    {
        int q = ((quarter_turns % 4) + 4) % 4;
        if (q % 2 == 1)
            return {base.height, base.width};
        return base;
    }

    // Analytic bounding box of `size` rotated by `angle` degrees about its
    // center (the rotated content's extent).
    inline Size analyticRotatedBounds(Size size, double angle_degrees)
    {
        if (std::fabs(angle_degrees) < 1e-9)
            return size;
        double a = degreesToRadians(angle_degrees);
        double c = std::fabs(std::cos(a));
        double s = std::fabs(std::sin(a));
        return {size.width * c + size.height * s, size.width * s + size.height * c};
    }

    // ImageMagick's canvas for that rotation (see the header comment): the
    // analytic extent plus one, rounded up to even. Identity for angle 0.
    inline double imageMagickCanvasAxis(double analytic)
    {
        double v = std::ceil(analytic + 1.0);
        if (std::fmod(v, 2.0) != 0.0)
            v += 1.0;
        return v;
    }

    inline Size rotatedBounds(Size size, double angle_degrees)
    {
        if (std::fabs(angle_degrees) < 1e-9)
            return size;
        Size a = analyticRotatedBounds(size, angle_degrees);
        return {imageMagickCanvasAxis(a.width), imageMagickCanvasAxis(a.height)};
    }

    // The rotated canvas: quarter turns first, then the fine angle.
    inline Size rotatedCanvas(Size base, int quarter_turns, double angle_degrees)
    {
        return rotatedBounds(afterQuarterTurns(base, quarter_turns), angle_degrees);
    }

    // Pixel size of a normalized crop applied to a canvas (rounded, >= 1).
    inline Size cropPixels(Size canvas, const NormRect &crop)
    {
        double w = std::round(canvas.width * crop.width);
        double h = std::round(canvas.height * crop.height);
        return {std::max(1.0, w), std::max(1.0, h)};
    }

    // True when every corner of `crop` (normalized on the rotated canvas of
    // `base` turned by `quarter_turns` + `angle`) lies inside the rotated
    // image content, i.e. the render has no empty corners. `tolerance` is a
    // fraction of the canvas (0.005 = half a percent) that absorbs the
    // sub-pixel differences between renderers.
    inline bool cropInsideContent(Size base, int quarter_turns, double angle_degrees,
                                  const NormRect &crop, double tolerance = 0.005)
    {
        Size turned = afterQuarterTurns(base, quarter_turns);
        if (turned.width <= 0 || turned.height <= 0)
            return false;
        Size canvas = rotatedBounds(turned, angle_degrees);
        if (crop.width <= 0 || crop.height <= 0)
            return false;

        double a = degreesToRadians(angle_degrees);
        double c = std::cos(a);
        double s = std::sin(a);
        double cx = canvas.width / 2.0;
        double cy = canvas.height / 2.0;
        double tol_x = tolerance * canvas.width;
        double tol_y = tolerance * canvas.height;

        const double xs[2] = {crop.left * canvas.width, crop.right() * canvas.width};
        const double ys[2] = {crop.top * canvas.height, crop.bottom() * canvas.height};
        for (double x : xs)
        {
            for (double y : ys)
            {
                // Undo the rotation about the canvas center: a canvas point
                // maps back into the un-rotated (turned) image frame.
                double dx = x - cx;
                double dy = y - cy;
                double ux = dx * c + dy * s;
                double uy = -dx * s + dy * c;
                if (std::fabs(ux) > turned.width / 2.0 + tol_x)
                    return false;
                if (std::fabs(uy) > turned.height / 2.0 + tol_y)
                    return false;
            }
        }
        return true;
    }

    // The largest centered rectangle of aspect `aspect` (width / height, in
    // canvas pixels) that fits inside the rotated image content, as a
    // normalized rect on the rotated canvas. aspect <= 0 means "same as the
    // turned base image". This is Lightroom's "constrain to image" fit.
    inline NormRect largestInscribedRect(Size base, int quarter_turns, double angle_degrees,
                                         double aspect = 0.0)
    {
        Size turned = afterQuarterTurns(base, quarter_turns);
        if (turned.width <= 0 || turned.height <= 0)
            return {};
        Size canvas = rotatedBounds(turned, angle_degrees);
        if (aspect <= 0)
            aspect = turned.width / turned.height;

        double a = degreesToRadians(angle_degrees);
        double c = std::fabs(std::cos(a));
        double s = std::fabs(std::sin(a));
        // Half-extents (x, y) with x = aspect * y must satisfy
        //   x*c + y*s <= w/2  and  x*s + y*c <= h/2.
        double y1 = (turned.width / 2.0) / (aspect * c + s);
        double y2 = (turned.height / 2.0) / (aspect * s + c);
        double y = std::min(y1, y2);
        double x = aspect * y;

        NormRect r;
        r.width = std::min(1.0, (2.0 * x) / canvas.width);
        r.height = std::min(1.0, (2.0 * y) / canvas.height);
        r.left = (1.0 - r.width) / 2.0;
        r.top = (1.0 - r.height) / 2.0;
        return r;
    }

    // Straighten angle that levels a line drawn from (x1,y1) to (x2,y2) in
    // canvas pixels: the line is made horizontal or vertical, whichever is
    // nearer. Result in degrees, clockwise positive, within [-45, 45].
    inline double levelingAngle(double x1, double y1, double x2, double y2)
    {
        double dx = x2 - x1;
        double dy = y2 - y1;
        if (std::fabs(dx) < 1e-9 && std::fabs(dy) < 1e-9)
            return 0.0;
        double deg = std::atan2(dy, dx) * 180.0 / kPi; // angle of the line
        // Fold into [-45, 45]: the nearest axis is the target.
        while (deg > 45.0)
            deg -= 90.0;
        while (deg < -45.0)
            deg += 90.0;
        // Rotating the image by -deg brings the line onto the axis.
        return -deg;
    }
}
