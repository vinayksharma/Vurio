#include "adjustments/tools/detail_tools.hpp"
#include "adjustments/chain_engine.hpp"
#include <algorithm>
#include <cmath>
#include <opencv2/imgproc.hpp>

namespace MediaDedup::Adjustments
{
    namespace
    {
        constexpr double kLumB = 0.0722, kLumG = 0.7152, kLumR = 0.2126;

        void clamp(Frame &io)
        {
            cv::min(io.pixels, 1.0f, io.pixels);
            cv::max(io.pixels, 0.0f, io.pixels);
        }

        cv::Mat luma(const cv::Mat &bgr)
        {
            cv::Mat l;
            cv::transform(bgr, l, cv::Matx13f(static_cast<float>(kLumB), static_cast<float>(kLumG), static_cast<float>(kLumR)));
            return l;
        }

        // He et al.'s guided filter of `p` against the single-channel
        // guide `I`, box radius `r` px.
        cv::Mat guidedFilter(const cv::Mat &I, const cv::Mat &p, int r, double eps)
        {
            cv::Size k(2 * r + 1, 2 * r + 1);
            auto box = [&](const cv::Mat &m) {
                cv::Mat o;
                cv::boxFilter(m, o, CV_32F, k, cv::Point(-1, -1), true, cv::BORDER_REFLECT);
                return o;
            };
            cv::Mat mean_I = box(I), mean_p = box(p);
            cv::Mat corr_I = box(I.mul(I)), corr_Ip = box(I.mul(p));
            cv::Mat var_I = corr_I - mean_I.mul(mean_I);
            cv::Mat cov_Ip = corr_Ip - mean_I.mul(mean_p);
            cv::Mat a = cov_Ip / (var_I + static_cast<float>(eps));
            cv::Mat b = mean_p - a.mul(mean_I);
            return box(a).mul(I) + box(b);
        }
    }

    // ---------------------------------------------------------------- grain

    GrainTool::GrainTool()
    {
        descriptor_.id = kId;
        descriptor_.label = "Grain";
        descriptor_.category = ToolCategory::Layer;
        descriptor_.icon = "circle.grid.3x3.fill";
        descriptor_.summary = "{amount:.2f} · {size:.4f}";
        descriptor_.resolution_dependent = true;
        number("amount", "Amount", 0, 1, 0, 0.01);
        number("size", "Size (of short side)", 0.001, 0.02, 0.004, 0.0005);
        number("color", "Colour", 0, 1, 0, 0.01);
        integer("seed", "Seed", 0, 9999, 1);
    }

    bool GrainTool::isIdentity(const ToolState &state) const { return as(state).number("amount") <= 0; }

    void GrainTool::fields(int width, int height, double cell_px, int seed, cv::Mat &mono, cv::Mat &colour)
    {
        double cell = std::max(cell_px, 1e-6);
        int cw = std::max(1, std::min(width, static_cast<int>(std::lround(width / cell))));
        int ch = std::max(1, std::min(height, static_cast<int>(std::lround(height / cell))));
        cv::RNG rng(static_cast<uint64_t>(seed) * 2654435761ULL + 12345ULL);
        cv::Mat m(ch, cw, CV_32FC1), c(ch, cw, CV_32FC3);
        rng.fill(m, cv::RNG::NORMAL, 0.0, 1.0);
        rng.fill(c, cv::RNG::NORMAL, 0.0, 1.0);
        int interp = (cw < width || ch < height) ? cv::INTER_CUBIC : cv::INTER_NEAREST;
        cv::resize(m, mono, cv::Size(width, height), 0, 0, interp);
        cv::resize(c, colour, cv::Size(width, height), 0, 0, interp);
    }

    void GrainTool::apply(const ToolState &state, Frame &io, const RenderContext &ctx) const
    {
        const ParamState &s = as(state);
        double amount = s.number("amount");
        if (amount <= 0 || io.empty())
            return;
        double size = s.number("size", 0.004), colour_mix = s.number("color");
        int seed = static_cast<int>(std::lround(s.number("seed", 1)));
        cv::Mat mono, colour;
        fields(io.width(), io.height(), size * ctx.pixel_scale, seed, mono, colour);
        const float k = static_cast<float>(amount * 0.2);
        const float cm = static_cast<float>(colour_mix);
        for (int y = 0; y < io.height(); ++y)
        {
            cv::Vec3f *row = io.pixels.ptr<cv::Vec3f>(y);
            const float *m = mono.ptr<float>(y);
            const cv::Vec3f *c = colour.ptr<cv::Vec3f>(y);
            for (int x = 0; x < io.width(); ++x)
            {
                float L = static_cast<float>(kLumB) * row[x][0] + static_cast<float>(kLumG) * row[x][1] + static_cast<float>(kLumR) * row[x][2];
                float w = k * (1.0f - 0.5f * L);
                for (int ch = 0; ch < 3; ++ch)
                    row[x][ch] += w * (m[x] * (1 - cm) + c[x][ch] * cm);
            }
        }
        clamp(io);
    }

    // -------------------------------------------------------------- clarity

    ClarityTool::ClarityTool()
    {
        descriptor_.id = kId;
        descriptor_.label = "Clarity";
        descriptor_.category = ToolCategory::Raster;
        descriptor_.icon = "circle.lefthalf.filled.righthalf.striped.horizontal";
        descriptor_.summary = "{amount:+.2f}";
        descriptor_.resolution_dependent = true;
        number("amount", "Amount", -1, 1, 0, 0.01);
        number("radius", "Radius (of short side)", 0.01, 0.2, 0.05, 0.005);
    }

    void ClarityTool::apply(const ToolState &state, Frame &io, const RenderContext &ctx) const
    {
        const ParamState &s = as(state);
        double amount = s.number("amount");
        if (std::fabs(amount) < 1e-9 || io.empty())
            return;
        double sigma = std::max(0.5, s.number("radius", 0.05) * ctx.pixel_scale);
        cv::Mat L = luma(io.pixels);
        cv::Mat blurred = ChainEngine::gaussianBlur(L, sigma);
        const float k = static_cast<float>(amount * 2.0);
        for (int y = 0; y < io.height(); ++y)
        {
            cv::Vec3f *row = io.pixels.ptr<cv::Vec3f>(y);
            const float *l = L.ptr<float>(y);
            const float *b = blurred.ptr<float>(y);
            for (int x = 0; x < io.width(); ++x)
            {
                float w = 4.0f * l[x] * (1.0f - l[x]);
                float d = k * (l[x] - b[x]) * w;
                row[x][0] += d;
                row[x][1] += d;
                row[x][2] += d;
            }
        }
        clamp(io);
    }

    // --------------------------------------------------------------- dehaze

    DehazeTool::DehazeTool()
    {
        descriptor_.id = kId;
        descriptor_.label = "Dehaze";
        descriptor_.category = ToolCategory::Raster;
        descriptor_.icon = "cloud.sun";
        descriptor_.summary = "{amount:+.2f}";
        descriptor_.resolution_dependent = true;
        number("amount", "Amount", -1, 1, 0, 0.01);
    }

    cv::Vec3f DehazeTool::atmosphericLight(const cv::Mat &pixels, const cv::Mat &dark)
    {
        // Quantile on a reduced copy: the estimate needs no more precision.
        cv::Mat d = dark, p = pixels;
        int longest = std::max(d.cols, d.rows);
        if (longest > 256)
        {
            double f = 256.0 / longest;
            cv::resize(dark, d, cv::Size(), f, f, cv::INTER_AREA);
            cv::resize(pixels, p, d.size(), 0, 0, cv::INTER_AREA);
        }
        std::vector<float> values(d.begin<float>(), d.end<float>());
        if (values.empty())
            return cv::Vec3f(1, 1, 1);
        size_t keep = std::max<size_t>(1, values.size() / 1000);
        std::nth_element(values.begin(), values.begin() + static_cast<long>(values.size() - keep), values.end());
        float threshold = values[values.size() - keep];
        cv::Vec3d sum(0, 0, 0);
        int n = 0;
        for (int y = 0; y < d.rows; ++y)
            for (int x = 0; x < d.cols; ++x)
                if (d.at<float>(y, x) >= threshold)
                {
                    sum += cv::Vec3d(p.at<cv::Vec3f>(y, x));
                    ++n;
                }
        if (n == 0)
            return cv::Vec3f(1, 1, 1);
        return cv::Vec3f(static_cast<float>(sum[0] / n), static_cast<float>(sum[1] / n), static_cast<float>(sum[2] / n));
    }

    void DehazeTool::apply(const ToolState &state, Frame &io, const RenderContext &ctx) const
    {
        double amount = as(state).number("amount");
        if (std::fabs(amount) < 1e-9 || io.empty())
            return;
        // Dark channel: the channel minimum, min-filtered over the window.
        std::vector<cv::Mat> ch;
        cv::split(io.pixels, ch);
        cv::Mat dark;
        cv::min(ch[0], ch[1], dark);
        cv::min(dark, ch[2], dark);
        int r = std::max(1, static_cast<int>(std::lround(0.015 * ctx.pixel_scale)));
        cv::Mat kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(2 * r + 1, 2 * r + 1));
        cv::erode(dark, dark, kernel, cv::Point(-1, -1), 1, cv::BORDER_REPLICATE);
        cv::Vec3f A = atmosphericLight(io.pixels, dark);
        if (amount < 0)
        {
            // Add haze: move toward the atmospheric light.
            float h = static_cast<float>(-amount * 0.5);
            cv::Mat haze(io.pixels.size(), CV_32FC3, cv::Scalar(A[0], A[1], A[2]));
            cv::addWeighted(io.pixels, 1 - h, haze, h, 0, io.pixels);
            clamp(io);
            return;
        }
        float a_max = std::max(0.05f, std::max(A[0], std::max(A[1], A[2])));
        cv::Mat t = 1.0f - static_cast<float>(0.95 * amount) * dark / a_max;
        int gr = std::max(2, static_cast<int>(std::lround(0.04 * ctx.pixel_scale)));
        t = guidedFilter(luma(io.pixels), t, gr, 1e-3);
        cv::min(t, 1.0f, t);
        cv::max(t, 0.1f, t);
        for (int y = 0; y < io.height(); ++y)
        {
            cv::Vec3f *row = io.pixels.ptr<cv::Vec3f>(y);
            const float *tr = t.ptr<float>(y);
            for (int x = 0; x < io.width(); ++x)
                for (int c = 0; c < 3; ++c)
                    row[x][c] = (row[x][c] - A[c]) / tr[x] + A[c];
        }
        clamp(io);
    }
}
