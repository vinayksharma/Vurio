#include "adjustments/tools/perspective_tool.hpp"
#include <algorithm>
#include <cmath>
#include <opencv2/imgproc.hpp>

namespace MediaDedup::Adjustments
{
    namespace
    {
        cv::Point2d map(const cv::Matx33d &h, double x, double y)
        {
            double w = h(2, 0) * x + h(2, 1) * y + h(2, 2);
            if (std::fabs(w) < 1e-12)
                w = 1e-12;
            return {(h(0, 0) * x + h(0, 1) * y + h(0, 2)) / w, (h(1, 0) * x + h(1, 1) * y + h(1, 2)) / w};
        }

        // Inside a convex quad given in order (either winding).
        bool inside(const cv::Point2d quad[4], const cv::Point2d &p)
        {
            double sign = 0;
            for (int i = 0; i < 4; ++i)
            {
                const cv::Point2d &a = quad[i], &b = quad[(i + 1) % 4];
                double cross = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
                if (std::fabs(cross) < 1e-9)
                    continue;
                if (sign == 0)
                    sign = cross > 0 ? 1 : -1;
                else if ((cross > 0 ? 1 : -1) != sign)
                    return false;
            }
            return true;
        }
    }

    PerspectiveTool::PerspectiveTool()
    {
        descriptor_.id = kId;
        descriptor_.label = "Perspective";
        descriptor_.category = ToolCategory::Geometry;
        descriptor_.icon = "perspective";
        descriptor_.lane_allowed = false;
        descriptor_.summary = "v {vertical:+.2f} · h {horizontal:+.2f}";
        number("vertical", "Vertical", -1, 1, 0, 0.01);
        number("horizontal", "Horizontal", -1, 1, 0, 0.01);
        flag("auto_fill", "Fill the frame", true);
    }

    cv::Matx33d PerspectiveTool::keystone(double w, double h, double vertical, double horizontal)
    {
        double ax_t = std::max(0.0, vertical) * kPull * w;   // top corners in
        double ax_b = std::max(0.0, -vertical) * kPull * w;  // bottom corners in
        double ay_r = std::max(0.0, horizontal) * kPull * h; // right corners in
        double ay_l = std::max(0.0, -horizontal) * kPull * h;
        cv::Point2f src[4] = {{0, 0}, {static_cast<float>(w), 0}, {static_cast<float>(w), static_cast<float>(h)}, {0, static_cast<float>(h)}};
        cv::Point2f dst[4] = {{static_cast<float>(ax_t), static_cast<float>(ay_l)},
                              {static_cast<float>(w - ax_t), static_cast<float>(ay_r)},
                              {static_cast<float>(w - ax_b), static_cast<float>(h - ay_r)},
                              {static_cast<float>(ax_b), static_cast<float>(h - ay_l)}};
        cv::Mat m = cv::getPerspectiveTransform(src, dst);
        cv::Matx33d out;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                out(i, j) = m.at<double>(i, j);
        return out;
    }

    double PerspectiveTool::fillScale(double w, double h, const cv::Matx33d &hm)
    {
        cv::Point2d quad[4] = {map(hm, 0, 0), map(hm, w, 0), map(hm, w, h), map(hm, 0, h)};
        const double cx = w / 2, cy = h / 2;
        auto fits = [&](double s) {
            cv::Point2d corners[4] = {{cx - s * w / 2, cy - s * h / 2}, {cx + s * w / 2, cy - s * h / 2},
                                      {cx + s * w / 2, cy + s * h / 2}, {cx - s * w / 2, cy + s * h / 2}};
            for (const auto &c : corners)
                if (!inside(quad, c))
                    return false;
            return true;
        };
        if (fits(1.0))
            return 1.0;
        double lo = 0.0, hi = 1.0;
        for (int i = 0; i < 24; ++i)
        {
            double mid = (lo + hi) / 2;
            if (fits(mid))
                lo = mid;
            else
                hi = mid;
        }
        return std::max(lo, 0.05);
    }

    void PerspectiveTool::apply(const ToolState &state, Frame &io, const RenderContext &) const
    {
        const ParamState &s = as(state);
        double v = s.number("vertical"), hz = s.number("horizontal");
        if ((std::fabs(v) < 1e-9 && std::fabs(hz) < 1e-9) || io.empty())
            return;
        const double w = io.width(), h = io.height();
        cv::Matx33d hm = keystone(w, h, v, hz);
        if (s.number("auto_fill", 1) != 0)
        {
            // Scale the inscribed rectangle (centred, canvas aspect) up to
            // the canvas: translate its centre to the origin, scale by 1/s,
            // translate back.
            double sc = fillScale(w, h, hm);
            double k = 1.0 / sc;
            cv::Matx33d fill(k, 0, w / 2 - k * w / 2, 0, k, h / 2 - k * h / 2, 0, 0, 1);
            hm = fill * hm;
        }
        cv::Mat m(3, 3, CV_64F);
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                m.at<double>(i, j) = hm(i, j);
        cv::Mat out;
        cv::warpPerspective(io.pixels, out, m, cv::Size(io.width(), io.height()), cv::INTER_CUBIC, cv::BORDER_CONSTANT,
                            cv::Scalar(0, 0, 0));
        io.pixels = out;
        io.composeHomography(hm);
    }
}
