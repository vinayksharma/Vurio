#include "adjustments/tools/tone_tools.hpp"
#include <cstdio>
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

        // Even out per-pixel maps against the single-channel guide `I`: a
        // guided filter (a local linear fit map ≈ a·I + b over a box of
        // radius `r`) whose fit trusts a pixel less the closer it sits to
        // an edge of the guide. Edge pixels are blends of the two sides —
        // their map value belongs to neither — so the fit is made from the
        // settled pixels on either side and carried across the edge by
        // the guide itself.
        void smoothMaps(const cv::Mat &I, std::initializer_list<cv::Mat *> maps, int r)
        {
            constexpr float kEps = 1e-3f;
            auto box = [](const cv::Mat &m, int radius) {
                cv::Mat o;
                cv::boxFilter(m, o, CV_32F, cv::Size(2 * radius + 1, 2 * radius + 1), cv::Point(-1, -1), true,
                              cv::BORDER_REFLECT);
                return o;
            };
            const int rs = std::max(1, r / 2);
            cv::Mat m1 = box(I, rs);
            cv::Mat local_var = box(I.mul(I), rs) - m1.mul(m1);
            cv::max(local_var, 0.0f, local_var);
            cv::Mat w = kEps / (local_var + kEps);
            w = w.mul(w);
            w += 1e-4f;
            cv::Mat sum_w = box(w, r);
            auto wbox = [&](const cv::Mat &m) { return cv::Mat(box(w.mul(m), r) / sum_w); };
            cv::Mat mean_I = wbox(I);
            cv::Mat var_I = wbox(I.mul(I)) - mean_I.mul(mean_I);
            for (cv::Mat *p : maps)
            {
                if (p->empty())
                    continue;
                cv::Mat mean_p = wbox(*p);
                cv::Mat cov = wbox(I.mul(*p)) - mean_I.mul(mean_p);
                cv::Mat a = cov / (var_I + kEps);
                cv::Mat b = mean_p - a.mul(mean_I);
                *p = box(a, r).mul(I) + box(b, r);
            }
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
        descriptor_.label = "Color Mixer";
        descriptor_.category = ToolCategory::Layer;
        descriptor_.icon = "circle.hexagongrid.fill";
        descriptor_.summary = "8 colours";
        descriptor_.pick_hint = "Press on a colour in the picture and drag up or down";
        descriptor_.pick_drag = true;
        descriptor_.resolution_dependent = true;
        // One group per property, a slider per colour band — the mixer
        // layout: "what do I want to do to the blues" reads down a column
        // of colours. Each track shows what its slider does to that band.
        auto band = [this](int b, const char *suffix, double min, double max, double step, std::vector<std::string> track)
        {
            number((std::string(kBandIds[b]) + suffix).c_str(), kBandLabels[b], min, max, 0, step);
            descriptor_.params.back().track_colors = std::move(track);
        };
        group("Hue");
        for (int b = 0; b < kBands; ++b)
            band(b, "_hue", -45, 45, 1,
                 {hexOfHsv(kBandCentres[b] - 45, 0.85, 0.9), hexOfHsv(kBandCentres[b], 0.85, 0.9), hexOfHsv(kBandCentres[b] + 45, 0.85, 0.9)});
        group("Saturation");
        for (int b = 0; b < kBands; ++b)
            band(b, "_sat", -1, 1, 0.01, {hexOfHsv(kBandCentres[b], 0.0, 0.55), hexOfHsv(kBandCentres[b], 0.95, 0.9)});
        group("Luminance");
        for (int b = 0; b < kBands; ++b)
            band(b, "_lum", -1, 1, 0.01,
                 {hexOfHsv(kBandCentres[b], 0.9, 0.3), hexOfHsv(kBandCentres[b], 0.85, 0.9), hexOfHsv(kBandCentres[b], 0.35, 1.0)});
        group(nullptr);
    }

    bool HslTool::pick(const PickedColour &input, std::vector<std::pair<std::string, double>> &values, std::string &note,
                       std::string &error) const
    {
        (void)note;
        const double r = input.r, g = input.g, b = input.b;
        const double hi = std::max({r, g, b}), lo = std::min({r, g, b});
        // The engine fades its effect out below s = 0.25 and is off at 0;
        // under a tenth there is nothing a drag could visibly change.
        if (hi < 0.02 || (hi - lo) / hi < 0.1)
        {
            error = "That spot has no colour to adjust; press on something more colourful";
            return false;
        }
        double hue;
        const double d = hi - lo;
        if (hi == r)
            hue = 60.0 * std::fmod((g - b) / d, 6.0);
        else if (hi == g)
            hue = 60.0 * ((b - r) / d + 2.0);
        else
            hue = 60.0 * ((r - g) / d + 4.0);
        double w[kBands];
        weights(hue, w);
        values.clear();
        for (int k = 0; k < kBands; ++k)
        {
            if (w[k] <= 0.005)
                continue;
            for (const char *suffix : {"_hue", "_sat", "_lum"})
                values.emplace_back(std::string(kBandIds[k]) + suffix, w[k]);
        }
        return true;
    }

    std::string HslTool::hexOfHsv(double hue_degrees, double s, double v)
    {
        double h = std::fmod(std::fmod(hue_degrees, 360.0) + 360.0, 360.0) / 60.0;
        double c = v * s, x = c * (1 - std::fabs(std::fmod(h, 2.0) - 1)), m = v - c;
        double r = 0, g = 0, b = 0;
        switch (static_cast<int>(h) % 6)
        {
        case 0: r = c, g = x; break;
        case 1: r = x, g = c; break;
        case 2: g = c, b = x; break;
        case 3: g = x, b = c; break;
        case 4: r = x, b = c; break;
        default: r = c, b = x; break;
        }
        char text[8];
        std::snprintf(text, sizeof text, "#%02X%02X%02X", static_cast<int>(std::lround((r + m) * 255)),
                      static_cast<int>(std::lround((g + m) * 255)), static_cast<int>(std::lround((b + m) * 255)));
        return text;
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

    void HslTool::apply(const ToolState &state, Frame &io, const RenderContext &ctx) const
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
        bool any_h = false, any_s = false, any_l = false;
        for (int b = 0; b < kBands; ++b)
        {
            any_h = any_h || hue[b] != 0;
            any_s = any_s || sat[b] != 0;
            any_l = any_l || lum[b] != 0;
        }
        if (!any_h && !any_s && !any_l)
            return;
        cv::Mat hsv;
        cv::cvtColor(io.pixels, hsv, cv::COLOR_BGR2HSV); // float: H in [0,360), S and V in [0,1]
        // How far each pixel moves, as three maps. A pixel's own hue is the
        // noisiest thing about it (chroma is subsampled and block-coded, and
        // an edge pixel is a blend whose hue belongs to neither side), so
        // moving each pixel by its own hue prints that noise into the
        // picture and leaves a rim along coloured edges. The maps are
        // therefore smoothed against the picture's luma before use: even
        // inside a region, stepping where the picture has an edge.
        cv::Mat mh, ms, ml;
        if (any_h)
            mh = cv::Mat::zeros(hsv.size(), CV_32F);
        if (any_s)
            ms = cv::Mat::zeros(hsv.size(), CV_32F);
        if (any_l)
            ml = cv::Mat::zeros(hsv.size(), CV_32F);
        for (int y = 0; y < hsv.rows; ++y)
        {
            const cv::Vec3f *row = hsv.ptr<cv::Vec3f>(y);
            float *rh = any_h ? mh.ptr<float>(y) : nullptr;
            float *rs = any_s ? ms.ptr<float>(y) : nullptr;
            float *rl = any_l ? ml.ptr<float>(y) : nullptr;
            for (int x = 0; x < hsv.cols; ++x)
            {
                float f = static_cast<float>(smoothstep(0.0, 0.25, row[x][1]));
                if (f <= 0)
                    continue;
                int i = static_cast<int>(row[x][0] * 10.0f);
                i = std::min(kSteps - 1, std::max(0, i));
                if (rh)
                    rh[x] = f * dh[static_cast<size_t>(i)];
                if (rs)
                    rs[x] = f * ds[static_cast<size_t>(i)];
                if (rl)
                    rl[x] = f * dl[static_cast<size_t>(i)];
            }
        }
        {
            cv::Mat guide;
            cv::transform(io.pixels, guide, cv::Matx13f(0.0722f, 0.7152f, 0.2126f));
            const int r = std::max(2, static_cast<int>(std::lround(kMapRadius * ctx.pixel_scale)));
            smoothMaps(guide, {&mh, &ms, &ml}, r);
        }
        for (int y = 0; y < hsv.rows; ++y)
        {
            cv::Vec3f *row = hsv.ptr<cv::Vec3f>(y);
            const float *rh = any_h ? mh.ptr<float>(y) : nullptr;
            const float *rs = any_s ? ms.ptr<float>(y) : nullptr;
            const float *rl = any_l ? ml.ptr<float>(y) : nullptr;
            for (int x = 0; x < hsv.cols; ++x)
            {
                float h = row[x][0], sv = row[x][1], v = row[x][2];
                // A grey has no hue to turn and no colour to scale; its
                // luminance follows the map like any other pixel's.
                float nh = h + (rh ? rh[x] : 0.0f);
                nh = std::fmod(std::fmod(nh, 360.0f) + 360.0f, 360.0f);
                float ns = std::min(1.0f, std::max(0.0f, sv * (1 + (rs ? rs[x] : 0.0f))));
                float l = std::min(1.0f, std::max(-1.0f, rl ? rl[x] : 0.0f)) * 0.5f;
                float nv = l >= 0 ? v + (1 - v) * l : v * (1 + l);
                row[x] = cv::Vec3f(nh, ns, std::min(1.0f, std::max(0.0f, nv)));
            }
        }
        cv::cvtColor(hsv, io.pixels, cv::COLOR_HSV2BGR);
        clamp(io);
    }
}
