#include "adjustments/tools/layer_tools.hpp"
#include "adjustments/chain_engine.hpp"
#include <algorithm>
#include <cmath>
#include <opencv2/imgproc.hpp>
#include <opencv2/photo.hpp>

namespace MediaDedup::Adjustments
{
    namespace
    {
        constexpr double kLumB = 0.0722, kLumG = 0.7152, kLumR = 0.2126;

        // A 3x3 colour matrix on BGR rows/cols from an RGB-ordered matrix.
        cv::Mat bgrMatrix(const double rgb[3][3])
        {
            // out_rgb = M · in_rgb. Reorder both axes to BGR: index map r=2,g=1,b=0.
            cv::Mat m(3, 3, CV_64F);
            const int map[3] = {2, 1, 0};
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    m.at<double>(i, j) = rgb[map[i]][map[j]];
            return m;
        }

        void applyMatrix(Frame &io, const cv::Mat &bgr3x3)
        {
            cv::Mat m32;
            bgr3x3.convertTo(m32, CV_32F);
            cv::transform(io.pixels, io.pixels, m32);
            cv::min(io.pixels, 1.0f, io.pixels);
            cv::max(io.pixels, 0.0f, io.pixels);
        }

        void clamp(Frame &io)
        {
            cv::min(io.pixels, 1.0f, io.pixels);
            cv::max(io.pixels, 0.0f, io.pixels);
        }
    }

    cv::Vec3f hsvToBgr(double h, double s, double v)
    {
        h = std::fmod(std::fmod(h, 360.0) + 360.0, 360.0) / 60.0;
        int i = static_cast<int>(std::floor(h));
        double f = h - i;
        double p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
        double r, g, b;
        switch (i % 6)
        {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
        }
        return cv::Vec3f(static_cast<float>(b), static_cast<float>(g), static_cast<float>(r));
    }

    // ------------------------------------------------------------ exposure

    ExposureTool::ExposureTool()
    {
        descriptor_.id = kId;
        descriptor_.label = "Exposure";
        descriptor_.category = ToolCategory::Layer;
        descriptor_.icon = "sun.max";
        descriptor_.summary = "{ev:+.2f} EV";
        number("ev", "Exposure (EV)", -3, 3, 0, 0.05);
    }

    void ExposureTool::apply(const ToolState &state, Frame &io, const RenderContext &) const
    {
        double k = std::pow(2.0, as(state).number("ev"));
        io.pixels.convertTo(io.pixels, CV_32FC3, k);
        clamp(io);
    }

    // ------------------------------------------------- brightness / contrast

    BrightContrastTool::BrightContrastTool()
    {
        descriptor_.id = kId;
        descriptor_.label = "Brightness / Contrast";
        descriptor_.category = ToolCategory::Layer;
        descriptor_.icon = "circle.lefthalf.filled";
        descriptor_.summary = "{brightness:+.2f} · {contrast:+.2f}";
        number("brightness", "Brightness", -0.5, 0.5, 0, 0.01);
        number("contrast", "Contrast", -1, 1, 0, 0.01);
    }

    void BrightContrastTool::apply(const ToolState &state, Frame &io, const RenderContext &) const
    {
        double b = as(state).number("brightness");
        double c = 1.0 + as(state).number("contrast");
        io.pixels.convertTo(io.pixels, CV_32FC3, c, 0.5 - 0.5 * c + b);
        clamp(io);
    }

    // -------------------------------------------------------- white balance

    WhiteBalanceTool::WhiteBalanceTool()
    {
        descriptor_.id = kId;
        descriptor_.label = "White Balance";
        descriptor_.category = ToolCategory::Layer;
        descriptor_.icon = "thermometer.medium";
        descriptor_.summary = "temp {temperature:+.2f} · tint {tint:+.2f}";
        number("temperature", "Temperature", -1, 1, 0, 0.01);
        number("tint", "Tint", -1, 1, 0, 0.01);
    }

    void WhiteBalanceTool::gains(double temperature, double tint, double &gr, double &gg, double &gb)
    {
        gr = 1.0 + 0.3 * temperature;
        gb = 1.0 - 0.3 * temperature;
        gg = 1.0 - 0.2 * tint;
        double k = 1.0 / (kLumR * gr + kLumG * gg + kLumB * gb);
        gr *= k;
        gg *= k;
        gb *= k;
    }

    void WhiteBalanceTool::apply(const ToolState &state, Frame &io, const RenderContext &) const
    {
        double gr, gg, gb;
        gains(as(state).number("temperature"), as(state).number("tint"), gr, gg, gb);
        cv::multiply(io.pixels, cv::Scalar(gb, gg, gr), io.pixels);
        clamp(io);
    }

    // ----------------------------------------------------------- saturation

    SaturationTool::SaturationTool()
    {
        descriptor_.id = kId;
        descriptor_.label = "Saturation";
        descriptor_.category = ToolCategory::Layer;
        descriptor_.icon = "paintpalette";
        descriptor_.summary = "{amount:+.2f}";
        number("amount", "Saturation", -1, 1, 0, 0.01);
    }

    void SaturationTool::apply(const ToolState &state, Frame &io, const RenderContext &) const
    {
        double s = 1.0 + as(state).number("amount");
        double m[3][3];
        const double lum[3] = {kLumR, kLumG, kLumB};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                m[i][j] = (1 - s) * lum[j] + (i == j ? s : 0.0);
        applyMatrix(io, bgrMatrix(m));
    }

    // ------------------------------------------------------------------ hue

    HueTool::HueTool()
    {
        descriptor_.id = kId;
        descriptor_.label = "Hue";
        descriptor_.category = ToolCategory::Layer;
        descriptor_.icon = "circle.hexagongrid";
        descriptor_.summary = "{degrees:+.0f}°";
        number("degrees", "Hue shift", -180, 180, 0, 1);
    }

    void HueTool::apply(const ToolState &state, Frame &io, const RenderContext &) const
    {
        double a = as(state).number("degrees") * kPi / 180.0;
        double c = std::cos(a), s = std::sin(a);
        // RGB → YIQ, rotate I/Q, YIQ → RGB.
        const double toYIQ[3][3] = {{0.299, 0.587, 0.114}, {0.596, -0.274, -0.322}, {0.211, -0.523, 0.312}};
        const double toRGB[3][3] = {{1.0, 0.956, 0.621}, {1.0, -0.272, -0.647}, {1.0, -1.106, 1.703}};
        const double rot[3][3] = {{1, 0, 0}, {0, c, -s}, {0, s, c}};
        double tmp[3][3], m[3][3];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
            {
                tmp[i][j] = 0;
                for (int k = 0; k < 3; ++k)
                    tmp[i][j] += rot[i][k] * toYIQ[k][j];
            }
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
            {
                m[i][j] = 0;
                for (int k = 0; k < 3; ++k)
                    m[i][j] += toRGB[i][k] * tmp[k][j];
            }
        applyMatrix(io, bgrMatrix(m));
    }

    // ----------------------------------------------------------- monochrome

    MonochromeTool::MonochromeTool()
    {
        descriptor_.id = kId;
        descriptor_.label = "Monochrome";
        descriptor_.category = ToolCategory::Layer;
        descriptor_.icon = "circle.lefthalf.striped.horizontal";
        descriptor_.summary = "{preset}";
        choice("preset", "Filter", {"neutral", "red_filter", "orange_filter", "yellow_filter", "green_filter", "blue_filter", "custom"}, "neutral");
        number("red", "Red weight", 0, 1, 0.2126, 0.01);
        number("green", "Green weight", 0, 1, 0.7152, 0.01);
        number("blue", "Blue weight", 0, 1, 0.0722, 0.01);
        number("tint_hue", "Tint hue", 0, 360, 40, 1);
        number("tint_amount", "Tint amount", 0, 1, 0, 0.01);
        flag("enabled_mono", "Convert to monochrome", true);
        descriptor_.active_at_defaults = true;
    }

    bool MonochromeTool::isIdentity(const ToolState &state) const
    {
        return as(state).number("enabled_mono", 1) == 0;
    }

    void MonochromeTool::weights(const std::string &preset, double r, double g, double b, double &wr, double &wg, double &wb)
    {
        if (preset == "red_filter") { wr = 0.7; wg = 0.2; wb = 0.1; }
        else if (preset == "orange_filter") { wr = 0.6; wg = 0.3; wb = 0.1; }
        else if (preset == "yellow_filter") { wr = 0.4; wg = 0.5; wb = 0.1; }
        else if (preset == "green_filter") { wr = 0.2; wg = 0.7; wb = 0.1; }
        else if (preset == "blue_filter") { wr = 0.1; wg = 0.2; wb = 0.7; }
        else if (preset == "custom") { wr = r; wg = g; wb = b; }
        else { wr = kLumR; wg = kLumG; wb = kLumB; }
        double sum = wr + wg + wb;
        if (sum <= 1e-9) { wr = kLumR; wg = kLumG; wb = kLumB; sum = 1; }
        wr /= sum; wg /= sum; wb /= sum;
    }

    void MonochromeTool::apply(const ToolState &state, Frame &io, const RenderContext &) const
    {
        const ParamState &s = as(state);
        if (s.number("enabled_mono", 1) == 0)
            return;
        double wr, wg, wb;
        weights(s.string("preset"), s.number("red"), s.number("green"), s.number("blue"), wr, wg, wb);
        double a = s.number("tint_amount");
        cv::Vec3f tint = hsvToBgr(s.number("tint_hue"), 0.35, 1.0);
        // Rows: each output channel = grey × (1 − a + a·C_channel).
        double m[3][3];
        const double tintRgb[3] = {tint[2], tint[1], tint[0]};
        const double w[3] = {wr, wg, wb};
        for (int i = 0; i < 3; ++i)
        {
            double scale = 1.0 - a + a * tintRgb[i];
            for (int j = 0; j < 3; ++j)
                m[i][j] = scale * w[j];
        }
        applyMatrix(io, bgrMatrix(m));
    }

    // -------------------------------------------------------- colour filter

    ColorFilterTool::ColorFilterTool()
    {
        descriptor_.id = kId;
        descriptor_.label = "Colour Filter";
        descriptor_.category = ToolCategory::Layer;
        descriptor_.icon = "camera.filters";
        descriptor_.summary = "{hue:.0f}° · {density:.0%}";
        number("hue", "Hue", 0, 360, 40, 1);
        number("saturation", "Saturation", 0, 1, 0.5, 0.01);
        number("density", "Density", 0, 1, 0, 0.01);
    }

    void ColorFilterTool::apply(const ToolState &state, Frame &io, const RenderContext &) const
    {
        const ParamState &s = as(state);
        double d = s.number("density");
        cv::Vec3f c = hsvToBgr(s.number("hue"), s.number("saturation"), 1.0);
        double lumC = kLumB * c[0] + kLumG * c[1] + kLumR * c[2];
        double k = lumC > 1e-6 ? 1.0 / lumC : 1.0;
        // Per-channel gain: (1 − d) + d·C·k, on the diagonal.
        cv::Scalar gain((1 - d) + d * c[0] * k, (1 - d) + d * c[1] * k, (1 - d) + d * c[2] * k);
        cv::multiply(io.pixels, gain, io.pixels);
        clamp(io);
    }

    // ---------------------------------------------------------------- gamma

    GammaTool::GammaTool()
    {
        descriptor_.id = kId;
        descriptor_.label = "Gamma";
        descriptor_.category = ToolCategory::Layer;
        descriptor_.icon = "waveform.path";
        descriptor_.summary = "γ {gamma:.2f}";
        number("gamma", "Gamma", 0.3, 3, 1, 0.01);
    }

    void GammaTool::apply(const ToolState &state, Frame &io, const RenderContext &) const
    {
        double g = as(state).number("gamma", 1);
        cv::max(io.pixels, 0.0f, io.pixels);
        cv::pow(io.pixels, 1.0 / g, io.pixels);
        clamp(io);
    }

    // ------------------------------------------------------------- vignette

    VignetteTool::VignetteTool()
    {
        descriptor_.id = kId;
        descriptor_.label = "Vignette";
        descriptor_.category = ToolCategory::Layer;
        descriptor_.icon = "circle.dashed";
        descriptor_.summary = "{amount:+.2f}";
        descriptor_.resolution_dependent = true;
        number("amount", "Amount", -1, 1, 0, 0.01);
        number("midpoint", "Midpoint", 0, 1, 0.5, 0.01);
        number("roundness", "Roundness", 0, 1, 0, 0.01);
        number("feather", "Feather", 0, 1, 0.5, 0.01);
    }

    float VignetteTool::falloff(double nx, double ny, double w, double h, double midpoint, double roundness, double feather)
    {
        // nx, ny in [-1, 1] across the frame.
        double rEll = std::sqrt(nx * nx + ny * ny);
        double shorter = std::min(w, h);
        double rCirc = std::sqrt((nx * w / 2) * (nx * w / 2) + (ny * h / 2) * (ny * h / 2)) / (shorter / 2);
        double r = rEll + (rCirc - rEll) * roundness;
        double lo = midpoint * 1.4142;
        double hi = lo + std::max(feather, 0.01) * 1.4142;
        double t = (r - lo) / (hi - lo);
        t = std::clamp(t, 0.0, 1.0);
        return static_cast<float>(t * t * (3 - 2 * t));
    }

    void VignetteTool::apply(const ToolState &state, Frame &io, const RenderContext &) const
    {
        const ParamState &s = as(state);
        double amount = s.number("amount");
        if (std::fabs(amount) < 1e-9 || io.empty())
            return;
        double midpoint = s.number("midpoint", 0.5), roundness = s.number("roundness"), feather = s.number("feather", 0.5);
        const int w = io.width(), h = io.height();
        for (int y = 0; y < h; ++y)
        {
            double ny = (2.0 * (y + 0.5) / h) - 1.0;
            cv::Vec3f *row = io.pixels.ptr<cv::Vec3f>(y);
            for (int x = 0; x < w; ++x)
            {
                double nx = (2.0 * (x + 0.5) / w) - 1.0;
                float g = 1.0f - static_cast<float>(amount) * falloff(nx, ny, w, h, midpoint, roundness, feather);
                row[x] *= g;
            }
        }
        clamp(io);
    }

    // -------------------------------------------------------------- sharpen

    SharpenTool::SharpenTool()
    {
        descriptor_.id = kId;
        descriptor_.label = "Sharpen";
        descriptor_.category = ToolCategory::Raster;
        descriptor_.icon = "triangle";
        descriptor_.summary = "{amount:.2f} · r {radius:.4f}";
        descriptor_.resolution_dependent = true;
        number("amount", "Amount", 0, 2, 0, 0.05);
        number("radius", "Radius (of short side)", 0.0005, 0.01, 0.002, 0.0005);
        number("threshold", "Threshold", 0, 0.2, 0, 0.005);
    }

    void SharpenTool::apply(const ToolState &state, Frame &io, const RenderContext &ctx) const
    {
        const ParamState &s = as(state);
        double amount = s.number("amount");
        if (amount <= 0 || io.empty())
            return;
        double sigma = std::max(0.3, s.number("radius", 0.002) * ctx.pixel_scale);
        cv::Mat blurred = ChainEngine::gaussianBlur(io.pixels, sigma);
        cv::Mat diff = io.pixels - blurred;
        double threshold = s.number("threshold");
        if (threshold > 0)
        {
            cv::Mat mask = cv::abs(diff) > static_cast<float>(threshold); // per channel, 0/255
            cv::Mat maskf;
            mask.convertTo(maskf, CV_32FC3, 1.0 / 255.0);
            cv::multiply(diff, maskf, diff);
        }
        io.pixels += diff * amount;
        clamp(io);
    }

    // -------------------------------------------------------------- denoise

    DenoiseTool::DenoiseTool()
    {
        descriptor_.id = kId;
        descriptor_.label = "Denoise";
        descriptor_.category = ToolCategory::Raster;
        descriptor_.icon = "sparkles";
        descriptor_.summary = "lum {luminance:.2f} · col {color:.2f}";
        number("luminance", "Luminance", 0, 1, 0, 0.05);
        number("color", "Colour", 0, 1, 0, 0.05);
    }

    void DenoiseTool::apply(const ToolState &state, Frame &io, const RenderContext &) const
    {
        const ParamState &s = as(state);
        double lum = s.number("luminance"), col = s.number("color");
        if ((lum <= 0 && col <= 0) || io.empty())
            return;
        cv::Mat bgr8 = ChainEngine::toBgr8(io);
        cv::Mat out8;
        cv::fastNlMeansDenoisingColored(bgr8, out8, static_cast<float>(3 + 12 * lum), static_cast<float>(3 + 12 * col), 7, 21);
        io.pixels = ChainEngine::fromBgr8(out8).pixels;
    }
}
