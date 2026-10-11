#include "adjustments/blend.hpp"
#include <algorithm>
#include <cmath>

namespace MediaDedup::Adjustments::Blend
{
    namespace
    {
        enum class Mode
        {
            Normal, Multiply, Screen, Overlay, SoftLight, HardLight, Darken, Lighten, Difference, Exclusion,
            ColorBurn, ColorDodge, Hue, Saturation, Color, Luminosity, Unknown
        };

        const std::vector<std::pair<std::string, Mode>> &table()
        {
            static const std::vector<std::pair<std::string, Mode>> t = {
                {"normal", Mode::Normal},         {"multiply", Mode::Multiply},   {"screen", Mode::Screen},
                {"overlay", Mode::Overlay},       {"soft_light", Mode::SoftLight}, {"hard_light", Mode::HardLight},
                {"darken", Mode::Darken},         {"lighten", Mode::Lighten},     {"difference", Mode::Difference},
                {"exclusion", Mode::Exclusion},   {"color_burn", Mode::ColorBurn}, {"color_dodge", Mode::ColorDodge},
                {"hue", Mode::Hue},               {"saturation", Mode::Saturation}, {"color", Mode::Color},
                {"luminosity", Mode::Luminosity}};
            return t;
        }

        Mode modeOf(const std::string &name)
        {
            for (const auto &p : table())
                if (p.first == name)
                    return p.second;
            return Mode::Unknown;
        }

        inline float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

        inline float hardLight(float b, float t)
        {
            return t <= 0.5f ? 2.f * t * b : 1.f - 2.f * (1.f - t) * (1.f - b);
        }

        inline float softLight(float b, float t)
        {
            if (t <= 0.5f)
                return b - (1.f - 2.f * t) * b * (1.f - b);
            float d = b <= 0.25f ? ((16.f * b - 12.f) * b + 4.f) * b : std::sqrt(b);
            return b + (2.f * t - 1.f) * (d - b);
        }

        inline float separable(Mode m, float b, float t)
        {
            switch (m)
            {
            case Mode::Normal: return t;
            case Mode::Multiply: return b * t;
            case Mode::Screen: return b + t - b * t;
            case Mode::Overlay: return hardLight(t, b);
            case Mode::SoftLight: return softLight(b, t);
            case Mode::HardLight: return hardLight(b, t);
            case Mode::Darken: return std::min(b, t);
            case Mode::Lighten: return std::max(b, t);
            case Mode::Difference: return std::fabs(b - t);
            case Mode::Exclusion: return b + t - 2.f * b * t;
            case Mode::ColorBurn:
                if (b >= 1.f) return 1.f;
                if (t <= 0.f) return 0.f;
                return 1.f - std::min(1.f, (1.f - b) / t);
            case Mode::ColorDodge:
                if (b <= 0.f) return 0.f;
                if (t >= 1.f) return 1.f;
                return std::min(1.f, b / (1.f - t));
            default: return t;
            }
        }

        // W3C non-separable helpers on RGB (the frame is BGR; callers swap).
        inline float lum(const cv::Vec3f &c) { return 0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2]; }

        inline cv::Vec3f clipColor(cv::Vec3f c)
        {
            float l = lum(c);
            float n = std::min({c[0], c[1], c[2]});
            float x = std::max({c[0], c[1], c[2]});
            if (n < 0.f)
                for (int i = 0; i < 3; ++i)
                    c[i] = l + (c[i] - l) * l / (l - n + 1e-12f);
            if (x > 1.f)
                for (int i = 0; i < 3; ++i)
                    c[i] = l + (c[i] - l) * (1.f - l) / (x - l + 1e-12f);
            return c;
        }

        inline cv::Vec3f setLum(cv::Vec3f c, float l)
        {
            float d = l - lum(c);
            return clipColor(cv::Vec3f(c[0] + d, c[1] + d, c[2] + d));
        }

        inline float sat(const cv::Vec3f &c) { return std::max({c[0], c[1], c[2]}) - std::min({c[0], c[1], c[2]}); }

        inline cv::Vec3f setSat(cv::Vec3f c, float s)
        {
            int imax = 0, imin = 0;
            for (int i = 1; i < 3; ++i)
            {
                if (c[i] > c[imax]) imax = i;
                if (c[i] < c[imin]) imin = i;
            }
            if (imax == imin)
                return cv::Vec3f(0, 0, 0);
            int imid = 3 - imax - imin;
            cv::Vec3f out(0, 0, 0);
            float range = c[imax] - c[imin];
            out[imid] = (c[imid] - c[imin]) * s / range;
            out[imax] = s;
            out[imin] = 0.f;
            return out;
        }

        inline cv::Vec3f rgb(const cv::Vec3f &bgr) { return cv::Vec3f(bgr[2], bgr[1], bgr[0]); }

        cv::Vec3f nonSeparable(Mode m, const cv::Vec3f &b_bgr, const cv::Vec3f &t_bgr)
        {
            cv::Vec3f b = rgb(b_bgr), t = rgb(t_bgr), r;
            switch (m)
            {
            case Mode::Hue: r = setLum(setSat(t, sat(b)), lum(b)); break;
            case Mode::Saturation: r = setLum(setSat(b, sat(t)), lum(b)); break;
            case Mode::Color: r = setLum(t, lum(b)); break;
            case Mode::Luminosity: r = setLum(b, lum(t)); break;
            default: r = t; break;
            }
            return rgb(r);
        }

        bool isSeparable(Mode m)
        {
            return m != Mode::Hue && m != Mode::Saturation && m != Mode::Color && m != Mode::Luminosity;
        }
    }

    const std::vector<std::string> &modes()
    {
        static const std::vector<std::string> names = []
        {
            std::vector<std::string> v;
            for (const auto &p : table())
                v.push_back(p.first);
            return v;
        }();
        return names;
    }

    bool isMode(const std::string &name) { return modeOf(name) != Mode::Unknown; }

    cv::Vec3f blendPixel(const std::string &mode, const cv::Vec3f &b, const cv::Vec3f &t)
    {
        Mode m = modeOf(mode);
        if (isSeparable(m))
            return cv::Vec3f(clamp01(separable(m, b[0], t[0])), clamp01(separable(m, b[1], t[1])),
                             clamp01(separable(m, b[2], t[2])));
        cv::Vec3f r = nonSeparable(m, b, t);
        return cv::Vec3f(clamp01(r[0]), clamp01(r[1]), clamp01(r[2]));
    }

    void apply(const std::string &mode, const cv::Mat &base, const cv::Mat &top, cv::Mat &out)
    {
        Mode m = modeOf(mode);
        CV_Assert(base.type() == CV_32FC3 && top.type() == CV_32FC3 && base.size() == top.size());
        if (m == Mode::Normal || m == Mode::Unknown)
        {
            top.copyTo(out);
            return;
        }
        cv::Mat result(base.size(), CV_32FC3);
        const int rows = base.rows, cols = base.cols;
        if (isSeparable(m))
        {
            for (int y = 0; y < rows; ++y)
            {
                const float *pb = base.ptr<float>(y);
                const float *pt = top.ptr<float>(y);
                float *po = result.ptr<float>(y);
                for (int i = 0; i < cols * 3; ++i)
                    po[i] = clamp01(separable(m, pb[i], pt[i]));
            }
        }
        else
        {
            for (int y = 0; y < rows; ++y)
            {
                const cv::Vec3f *pb = base.ptr<cv::Vec3f>(y);
                const cv::Vec3f *pt = top.ptr<cv::Vec3f>(y);
                cv::Vec3f *po = result.ptr<cv::Vec3f>(y);
                for (int x = 0; x < cols; ++x)
                {
                    cv::Vec3f r = nonSeparable(m, pb[x], pt[x]);
                    po[x] = cv::Vec3f(clamp01(r[0]), clamp01(r[1]), clamp01(r[2]));
                }
            }
        }
        out = result;
    }
}
