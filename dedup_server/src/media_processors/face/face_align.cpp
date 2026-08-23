#include "media_processors/face/face_align.hpp"

#include <cmath>

namespace MediaDedup
{
    const std::array<double, 5> &FaceAlign::templateX()
    {
        // The InsightFace/ArcFace 112×112 reference template.
        static const std::array<double, 5> x = {38.2946, 73.5318, 56.0252, 41.5493, 70.7299};
        return x;
    }

    const std::array<double, 5> &FaceAlign::templateY()
    {
        static const std::array<double, 5> y = {51.6963, 51.5014, 71.7366, 92.3655, 92.2041};
        return y;
    }

    bool FaceAlign::similarityTransform(const std::array<double, 5> &src_x,
                                        const std::array<double, 5> &src_y,
                                        std::array<double, 4> &out)
    {
        // Closed-form least squares for x' = a*x - b*y + tx, y' = b*x + a*y + ty
        // (Umeyama with uniform scale, no reflection). Solve the 4x4 normal
        // equations, which decouple nicely for the similarity model.
        const auto &dst_x = templateX();
        const auto &dst_y = templateY();
        constexpr int n = 5;

        double sx = 0, sy = 0, dx = 0, dy = 0;
        for (int i = 0; i < n; ++i)
        {
            sx += src_x[i];
            sy += src_y[i];
            dx += dst_x[i];
            dy += dst_y[i];
        }
        sx /= n; sy /= n; dx /= n; dy /= n;

        double var = 0, cov_aa = 0, cov_bb = 0;
        for (int i = 0; i < n; ++i)
        {
            const double ux = src_x[i] - sx, uy = src_y[i] - sy;
            const double vx = dst_x[i] - dx, vy = dst_y[i] - dy;
            var += ux * ux + uy * uy;
            cov_aa += ux * vx + uy * vy; // Σ u·v      -> a * var
            cov_bb += ux * vy - uy * vx; // Σ u×v (z)  -> b * var
        }
        if (var < 1e-9)
            return false; // coincident points

        const double a = cov_aa / var;
        const double b = cov_bb / var;
        if (std::abs(a) < 1e-9 && std::abs(b) < 1e-9)
            return false; // degenerate (collinear at 90°)

        out[0] = a;
        out[1] = b;
        out[2] = dx - (a * sx - b * sy);
        out[3] = dy - (b * sx + a * sy);
        return true;
    }

    bool FaceAlign::alignFace(const ImageBuffer &image, const DetectedFace &face,
                              std::vector<std::uint8_t> &out_rgb)
    {
        if (!image.valid() || !face.has_landmarks)
            return false;

        std::array<double, 5> px, py;
        for (int i = 0; i < 5; ++i)
        {
            px[i] = face.landmarks.x[i] * image.width;
            py[i] = face.landmarks.y[i] * image.height;
        }
        std::array<double, 4> t;
        if (!similarityTransform(px, py, t))
            return false;

        // Invert (a,b,tx,ty) to map each OUTPUT pixel back into the source
        // for bilinear sampling: forward is x' = a x - b y + tx.
        const double det = t[0] * t[0] + t[1] * t[1];
        if (det < 1e-12)
            return false;
        const double ia = t[0] / det, ib = -t[1] / det;

        out_rgb.assign(static_cast<std::size_t>(kSize) * kSize * 3, 0);
        for (int oy = 0; oy < kSize; ++oy)
        {
            for (int ox = 0; ox < kSize; ++ox)
            {
                const double rx = ox - t[2], ry = oy - t[3];
                const double sxf = ia * rx - ib * ry;
                const double syf = ib * rx + ia * ry;
                if (sxf < 0 || syf < 0 || sxf >= image.width - 1 || syf >= image.height - 1)
                    continue; // out of source: leave black
                const int x0 = static_cast<int>(sxf), y0 = static_cast<int>(syf);
                const double fx = sxf - x0, fy = syf - y0;
                const std::uint8_t *p00 = &image.rgba[(static_cast<std::size_t>(y0) * image.width + x0) * 4];
                const std::uint8_t *p10 = p00 + 4;
                const std::uint8_t *p01 = p00 + static_cast<std::size_t>(image.width) * 4;
                const std::uint8_t *p11 = p01 + 4;
                std::uint8_t *dst = &out_rgb[(static_cast<std::size_t>(oy) * kSize + ox) * 3];
                for (int c = 0; c < 3; ++c)
                {
                    const double v = p00[c] * (1 - fx) * (1 - fy) + p10[c] * fx * (1 - fy) +
                                     p01[c] * (1 - fx) * fy + p11[c] * fx * fy;
                    dst[c] = static_cast<std::uint8_t>(v + 0.5);
                }
            }
        }
        return true;
    }
}
