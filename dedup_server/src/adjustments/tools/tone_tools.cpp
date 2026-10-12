#include "adjustments/tools/tone_tools.hpp"
#include <algorithm>
#include <cmath>
#include <opencv2/imgproc.hpp>

namespace MediaDedup::Adjustments
{
    namespace
    {
        void clamp(Frame &io)
        {
            cv::min(io.pixels, 1.0f, io.pixels);
            cv::max(io.pixels, 0.0f, io.pixels);
        }

        inline float lookup(const std::vector<float> &lut, float v)
        {
            const float last = static_cast<float>(lut.size() - 1);
            float x = std::min(std::max(v, 0.0f), 1.0f) * last;
            int i = static_cast<int>(x);
            if (i >= static_cast<int>(lut.size()) - 1)
                return lut.back();
            float t = x - static_cast<float>(i);
            return lut[static_cast<size_t>(i)] * (1 - t) + lut[static_cast<size_t>(i) + 1] * t;
        }

        inline double smoothstep(double e0, double e1, double x)
        {
            double t = std::min(1.0, std::max(0.0, (x - e0) / (e1 - e0)));
            return t * t * (3 - 2 * t);
        }
    }

    void applyLuts(Frame &io, const std::vector<float> &lut_b, const std::vector<float> &lut_g,
                   const std::vector<float> &lut_r)
    {
        if (io.empty())
            return;
        for (int y = 0; y < io.height(); ++y)
        {
            cv::Vec3f *row = io.pixels.ptr<cv::Vec3f>(y);
            for (int x = 0; x < io.width(); ++x)
            {
                row[x][0] = lookup(lut_b, row[x][0]);
                row[x][1] = lookup(lut_g, row[x][1]);
                row[x][2] = lookup(lut_r, row[x][2]);
            }
        }
    }

    // --------------------------------------------------------------- levels

    LevelsTool::LevelsTool()
    {
        descriptor_.id = kId;
        descriptor_.label = "Levels";
        descriptor_.category = ToolCategory::Layer;
        descriptor_.icon = "slider.horizontal.below.rectangle";
        descriptor_.summary = "{in_black:.2f}–{in_white:.2f} · γ {gamma:.2f}";
        number("in_black", "Input black", 0, 0.9, 0, 0.005);
        number("in_white", "Input white", 0.1, 1, 1, 0.005);
        number("gamma", "Midtones (γ)", 0.1, 10, 1, 0.01);
        number("out_black", "Output black", 0, 1, 0, 0.005);
        number("out_white", "Output white", 0, 1, 1, 0.005);
    }

    bool LevelsTool::validate(const ToolState &state, Size, std::string &error) const
    {
        const ParamState &s = as(state);
        if (s.number("in_black") >= s.number("in_white", 1) - 0.01)
        {
            error = "levels: in_black must be below in_white";
            return false;
        }
        return true;
    }

    double LevelsTool::map(double in, double ib, double iw, double gamma, double ob, double ow)
    {
        double t = (in - ib) / std::max(iw - ib, 1e-6);
        t = std::min(1.0, std::max(0.0, t));
        t = std::pow(t, 1.0 / std::max(gamma, 1e-6));
        return std::min(1.0, std::max(0.0, ob + t * (ow - ob)));
    }

    void LevelsTool::apply(const ToolState &state, Frame &io, const RenderContext &) const
    {
        const ParamState &s = as(state);
        const double ib = s.number("in_black"), iw = s.number("in_white", 1), g = s.number("gamma", 1);
        const double ob = s.number("out_black"), ow = s.number("out_white", 1);
        std::vector<float> lut(1024);
        for (size_t k = 0; k < lut.size(); ++k)
            lut[k] = static_cast<float>(map(static_cast<double>(k) / (lut.size() - 1), ib, iw, g, ob, ow));
        applyLuts(io, lut, lut, lut);
        clamp(io);
    }

    // --------------------------------------------------------------- curves

    CurvesTool::CurvesTool()
    {
        descriptor_.id = kId;
        descriptor_.label = "Curves";
        descriptor_.category = ToolCategory::Layer;
        descriptor_.icon = "point.topleft.down.to.point.bottomright.curvepath";
        // The client renders an edited curve's label and nothing for a
        // straight one; "linear" when all four are straight.
        descriptor_.summary = "{rgb} {red} {green} {blue}";
        group("RGB");
        curve("rgb", "RGB");
        group("Red");
        curve("red", "Red");
        group("Green");
        curve("green", "Green");
        group("Blue");
        curve("blue", "Blue");
        group(nullptr);
    }

    void CurvesTool::apply(const ToolState &state, Frame &io, const RenderContext &) const
    {
        const ParamState &s = as(state);
        CurvePoints master = CurvePoints::fromJson(s.array("rgb"));
        const char *ids[3] = {"blue", "green", "red"};
        std::vector<float> luts[3];
        for (int c = 0; c < 3; ++c)
        {
            CurvePoints channel = CurvePoints::fromJson(s.array(ids[c]));
            luts[c].resize(kLutSize);
            for (int k = 0; k < kLutSize; ++k)
            {
                double x = static_cast<double>(k) / (kLutSize - 1);
                luts[c][static_cast<size_t>(k)] = static_cast<float>(channel.evaluate(master.evaluate(x)));
            }
        }
        applyLuts(io, luts[0], luts[1], luts[2]);
        clamp(io);
    }

    // ------------------------------------------------------------------ hsl

    const char *const HslTool::kBandIds[HslTool::kBands] = {"red", "orange", "yellow", "green", "aqua", "blue", "purple", "magenta"};
    const char *const HslTool::kBandLabels[HslTool::kBands] = {"Red", "Orange", "Yellow", "Green", "Aqua", "Blue", "Purple", "Magenta"};
    const double HslTool::kBandCentres[HslTool::kBands] = {0, 30, 60, 120, 180, 240, 280, 320};

    HslTool::HslTool()
    {
        descriptor_.id = kId;
        descriptor_.label = "HSL";
        descriptor_.category = ToolCategory::Layer;
        descriptor_.icon = "circle.hexagongrid.fill";
        descriptor_.summary = "8 bands";
        for (int b = 0; b < kBands; ++b)
        {
            group(kBandLabels[b]);
            std::string base = kBandIds[b];
            number((base + "_hue").c_str(), "Hue", -45, 45, 0, 1);
            number((base + "_sat").c_str(), "Saturation", -1, 1, 0, 0.01);
            number((base + "_lum").c_str(), "Luminance", -1, 1, 0, 0.01);
        }
        group(nullptr);
    }

    void HslTool::weights(double hue_degrees, double out[kBands])
    {
        double h = std::fmod(std::fmod(hue_degrees, 360.0) + 360.0, 360.0);
        for (int b = 0; b < kBands; ++b)
            out[b] = 0;
        for (int b = 0; b < kBands; ++b)
        {
            int next = (b + 1) % kBands;
            double c0 = kBandCentres[b], c1 = kBandCentres[next];
            if (next == 0)
                c1 += 360.0; // the magenta → red wrap
            if (h >= c0 && h < c1)
            {
                double t = (h - c0) / (c1 - c0);
                out[b] = 1 - t;
                out[next] = t;
                return;
            }
        }
        out[0] = 1; // h == 360 after fmod cannot happen, but keep the sum at 1
    }

    void HslTool::apply(const ToolState &state, Frame &io, const RenderContext &) const
    {
        if (io.empty())
            return;
        const ParamState &s = as(state);
        double hue[kBands], sat[kBands], lum[kBands];
        for (int b = 0; b < kBands; ++b)
        {
            std::string base = kBandIds[b];
            hue[b] = s.number(base + "_hue");
            sat[b] = s.number(base + "_sat");
            lum[b] = s.number(base + "_lum");
        }
        // The adjustment depends on the hue alone: tabulate it in 0.1° steps.
        constexpr int kSteps = 3600;
        std::vector<float> dh(kSteps), ds(kSteps), dl(kSteps);
        for (int i = 0; i < kSteps; ++i)
        {
            double w[kBands];
            weights(i / 10.0, w);
            double a = 0, b = 0, c = 0;
            for (int k = 0; k < kBands; ++k)
            {
                a += w[k] * hue[k];
                b += w[k] * sat[k];
                c += w[k] * lum[k];
            }
            dh[static_cast<size_t>(i)] = static_cast<float>(a);
            ds[static_cast<size_t>(i)] = static_cast<float>(b);
            dl[static_cast<size_t>(i)] = static_cast<float>(c);
        }
        cv::Mat hsv;
        cv::cvtColor(io.pixels, hsv, cv::COLOR_BGR2HSV); // float: H in [0,360), S and V in [0,1]
        for (int y = 0; y < hsv.rows; ++y)
        {
            cv::Vec3f *row = hsv.ptr<cv::Vec3f>(y);
            for (int x = 0; x < hsv.cols; ++x)
            {
                float h = row[x][0], sv = row[x][1], v = row[x][2];
                float f = static_cast<float>(smoothstep(0.0, 0.25, sv));
                if (f <= 0)
                    continue;
                int i = static_cast<int>(h * 10.0f);
                i = std::min(kSteps - 1, std::max(0, i));
                float nh = h + f * dh[static_cast<size_t>(i)];
                nh = std::fmod(std::fmod(nh, 360.0f) + 360.0f, 360.0f);
                float ns = std::min(1.0f, std::max(0.0f, sv * (1 + f * ds[static_cast<size_t>(i)])));
                float l = f * dl[static_cast<size_t>(i)] * 0.5f;
                float nv = l >= 0 ? v + (1 - v) * l : v * (1 + l);
                row[x] = cv::Vec3f(nh, ns, std::min(1.0f, std::max(0.0f, nv)));
            }
        }
        cv::cvtColor(hsv, io.pixels, cv::COLOR_HSV2BGR);
        clamp(io);
    }
}
