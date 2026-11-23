#include "adjustments/tools/look_tools.hpp"
#include "adjustments/blend.hpp"
#include "adjustments/chain_engine.hpp"
#include "adjustments/masks.hpp"
#include "adjustments/tools/layer_tools.hpp"
#include "adjustments/tools/tone_tools.hpp"
#include <algorithm>
#include <cmath>
#include <opencv2/imgproc.hpp>

namespace MediaDedup::Adjustments
{
    namespace
    {
        constexpr float kLumB = 0.0722f, kLumG = 0.7152f, kLumR = 0.2126f;

        void clamp(Frame &io)
        {
            cv::min(io.pixels, 1.0f, io.pixels);
            cv::max(io.pixels, 0.0f, io.pixels);
        }
    }

    // ----------------------------------------------------------------- blur

    BlurTool::BlurTool()
    {
        descriptor_.id = kId;
        descriptor_.label = "Blur";
        descriptor_.category = ToolCategory::Layer;
        descriptor_.icon = "drop";
        descriptor_.summary = "{radius:.3f}";
        descriptor_.resolution_dependent = true;
        number("radius", "Radius (of short side)", 0, 0.1, 0, 0.0005);
        flag("linear", "Blur as light (linear)", true);
    }

    void BlurTool::apply(const ToolState &state, Frame &io, const RenderContext &ctx) const
    {
        const ParamState &s = as(state);
        const double sigma = s.number("radius") * ctx.pixel_scale;
        if (io.empty() || sigma < 0.05)
            return;
        if (s.number("linear", 1) != 0)
        {
            Blend::toLinear(io.pixels);
            io.pixels = ChainEngine::gaussianBlur(io.pixels, sigma);
            Blend::toGamma(io.pixels);
            return;
        }
        io.pixels = ChainEngine::gaussianBlur(io.pixels, sigma);
        clamp(io);
    }

    // ------------------------------------------------------------- luma key

    LumaKeyTool::LumaKeyTool()
    {
        descriptor_.id = kId;
        descriptor_.label = "Luma Key";
        descriptor_.category = ToolCategory::Layer;
        descriptor_.icon = "circle.lefthalf.filled";
        descriptor_.summary = "{threshold:.2f}";
        descriptor_.active_at_defaults = true;
        choice("keep", "Keep", {"highlights", "shadows"}, "highlights");
        choice("key", "Measured by", {"brightest_channel", "luminance"}, "brightest_channel");
        number("threshold", "Threshold", 0, 1, 0.8, 0.01);
        number("softness", "Softness", 0, 0.5, 0.1, 0.01);
    }

    void LumaKeyTool::apply(const ToolState &state, Frame &io, const RenderContext &) const
    {
        const ParamState &s = as(state);
        const bool shadows = s.string("keep") == "shadows";
        const bool luma = s.string("key") == "luminance";
        const double th = s.number("threshold", 0.8), soft = s.number("softness", 0.1);
        for (int y = 0; y < io.height(); ++y)
        {
            cv::Vec3f *row = io.pixels.ptr<cv::Vec3f>(y);
            for (int x = 0; x < io.width(); ++x)
            {
                cv::Vec3f &p = row[x];
                float k = luma ? kLumB * p[0] + kLumG * p[1] + kLumR * p[2] : std::max({p[0], p[1], p[2]});
                float w = static_cast<float>(MaskMath::smoothstep(th - soft, th + soft, k));
                p *= shadows ? 1.0f - w : w;
            }
        }
    }

    // -------------------------------------------------------- channel mixer

    ChannelMixerTool::ChannelMixerTool()
    {
        descriptor_.id = kId;
        descriptor_.label = "Channel Mixer";
        descriptor_.category = ToolCategory::Layer;
        descriptor_.icon = "square.stack.3d.up";
        descriptor_.summary = "3 × 3";
        const char *outs[3] = {"red", "green", "blue"};
        const char *groups[3] = {"Red out", "Green out", "Blue out"};
        const char *labels[3] = {"From red", "From green", "From blue"};
        const char *ends[3] = {"#E63A2E", "#3FBF4A", "#3A6FE6"};
        for (int o = 0; o < 3; ++o)
        {
            group(groups[o]);
            for (int i = 0; i < 3; ++i)
            {
                std::string id = std::string(outs[o]) + "_" + "rgb"[i];
                number(id.c_str(), labels[i], -2, 2, o == i ? 1 : 0, 0.01);
                descriptor_.params.back().track_colors = {"#101010", ends[i]};
            }
        }
        group(nullptr);
    }

    void ChannelMixerTool::apply(const ToolState &state, Frame &io, const RenderContext &) const
    {
        const ParamState &s = as(state);
        // Rows and columns in the frame's B, G, R order.
        const char *names[3] = {"blue", "green", "red"};
        const char from[3] = {'b', 'g', 'r'};
        cv::Matx33f m;
        for (int o = 0; o < 3; ++o)
            for (int i = 0; i < 3; ++i)
                m(o, i) = static_cast<float>(s.number(std::string(names[o]) + "_" + from[i], o == i ? 1 : 0));
        cv::transform(io.pixels, io.pixels, m);
        clamp(io);
    }

    // --------------------------------------------------------- colour grade

    ColorGradeTool::ColorGradeTool()
    {
        descriptor_.id = kId;
        descriptor_.label = "Colour Grade";
        descriptor_.category = ToolCategory::Layer;
        descriptor_.icon = "circle.lefthalf.filled.inverse";
        descriptor_.summary = "shadows · mids · highlights";
        const char *ids[3] = {"shadows", "midtones", "highlights"};
        const char *groups[3] = {"Shadows", "Midtones", "Highlights"};
        const double hues[3] = {210, 40, 45};
        std::vector<std::string> rainbow;
        for (int h = 0; h <= 360; h += 60)
            rainbow.push_back(HslTool::hexOfHsv(h, 0.85, 0.9));
        for (int r = 0; r < 3; ++r)
        {
            group(groups[r]);
            std::string base = ids[r];
            number((base + "_hue").c_str(), "Hue", 0, 360, hues[r], 1);
            descriptor_.params.back().track_colors = rainbow;
            number((base + "_amount").c_str(), "Amount", 0, 1, 0, 0.01);
            number((base + "_lum").c_str(), "Luminance", -1, 1, 0, 0.01);
        }
        group(nullptr);
        number("balance", "Balance", -1, 1, 0, 0.01);
    }

    void ColorGradeTool::apply(const ToolState &state, Frame &io, const RenderContext &) const
    {
        const ParamState &s = as(state);
        const char *ids[3] = {"shadows", "midtones", "highlights"};
        cv::Vec3f tint[3];
        float lift[3];
        for (int r = 0; r < 3; ++r)
        {
            std::string base = ids[r];
            cv::Vec3f c = hsvToBgr(s.number(base + "_hue"), 1.0, 1.0);
            float lc = kLumB * c[0] + kLumG * c[1] + kLumR * c[2];
            float a = static_cast<float>(s.number(base + "_amount")) * 0.5f;
            tint[r] = cv::Vec3f((c[0] - lc) * a, (c[1] - lc) * a, (c[2] - lc) * a);
            lift[r] = static_cast<float>(s.number(base + "_lum")) * 0.25f;
        }
        const float power = static_cast<float>(std::pow(2.0, -s.number("balance")));
        for (int y = 0; y < io.height(); ++y)
        {
            cv::Vec3f *row = io.pixels.ptr<cv::Vec3f>(y);
            for (int x = 0; x < io.width(); ++x)
            {
                cv::Vec3f &p = row[x];
                float L = std::min(1.0f, std::max(0.0f, kLumB * p[0] + kLumG * p[1] + kLumR * p[2]));
                if (power != 1.0f)
                    L = std::pow(L, power);
                const float w[3] = {(1 - L) * (1 - L), 2 * L * (1 - L), L * L};
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 3; ++c)
                        p[c] += w[r] * (tint[r][c] + lift[r]);
            }
        }
        clamp(io);
    }
}
