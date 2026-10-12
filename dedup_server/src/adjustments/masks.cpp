#include "adjustments/masks.hpp"
#include "adjustments/chain_engine.hpp"
#include "adjustments/mask_service.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <opencv2/imgproc.hpp>

namespace MediaDedup::Adjustments
{
    namespace
    {
        constexpr double kLumB = 0.0722, kLumG = 0.7152, kLumR = 0.2126;

        void fillMask(const ToolDescriptor &, cv::Mat &out, const Frame &fork, float value)
        {
            out = cv::Mat(fork.height(), fork.width(), CV_32FC1, cv::Scalar(value));
        }

        // Whole frame.
        class FullMask final : public MaskSource
        {
        public:
            FullMask()
            {
                descriptor_.id = "full";
                descriptor_.label = "Whole image";
                descriptor_.category = ToolCategory::Mask;
                descriptor_.icon = "rectangle";
                descriptor_.summary = "whole image";
            }
            void coverage(const ToolState &, const Frame &fork, cv::Mat &out, const MaskContext &) const override { fillMask(descriptor_, out, fork, 1.0f); }
        };

        // 1 at p0, 0 at p1, a linear ramp between; both in base space.
        class LinearMask final : public MaskSource
        {
        public:
            LinearMask()
            {
                descriptor_.id = "linear";
                descriptor_.label = "Linear gradient";
                descriptor_.category = ToolCategory::Mask;
                descriptor_.icon = "square.lefthalf.filled";
                descriptor_.summary = "linear";
                number("x0", "Start x", 0, 1, 0.5, 0.001);
                number("y0", "Start y", 0, 1, 0.2, 0.001);
                number("x1", "End x", 0, 1, 0.5, 0.001);
                number("y1", "End y", 0, 1, 0.6, 0.001);
            }
            void coverage(const ToolState &state, const Frame &fork, cv::Mat &out, const MaskContext &) const override
            {
                const ParamState &s = as(state);
                cv::Point2d p0 = fork.project(s.number("x0", 0.5), s.number("y0", 0.2));
                cv::Point2d p1 = fork.project(s.number("x1", 0.5), s.number("y1", 0.6));
                cv::Point2d d = p1 - p0;
                double len2 = d.x * d.x + d.y * d.y;
                out = cv::Mat(fork.height(), fork.width(), CV_32FC1);
                for (int y = 0; y < fork.height(); ++y)
                {
                    float *row = out.ptr<float>(y);
                    for (int x = 0; x < fork.width(); ++x)
                    {
                        double t = len2 <= 1e-9 ? 0.0 : ((x + 0.5 - p0.x) * d.x + (y + 0.5 - p0.y) * d.y) / len2;
                        t = std::clamp(t, 0.0, 1.0);
                        row[x] = static_cast<float>(1.0 - t);
                    }
                }
            }
        private:
            void number(const char *id, const char *label, double min, double max, double def, double step)
            {
                schema_.number(id, label, min, max, def, step);
            }
        };

        // An ellipse in base space: centre, radii (fractions of the base's
        // shorter side), rotation; 1 inside, 0 outside, soft over `feather`.
        class RadialMask final : public MaskSource
        {
        public:
            RadialMask()
            {
                descriptor_.id = "radial";
                descriptor_.label = "Radial gradient";
                descriptor_.category = ToolCategory::Mask;
                descriptor_.icon = "circle.lefthalf.filled";
                descriptor_.summary = "radial";
                schema_.number("cx", "Centre x", 0, 1, 0.5, 0.001);
                schema_.number("cy", "Centre y", 0, 1, 0.5, 0.001);
                schema_.number("rx", "Radius x", 0.01, 2, 0.4, 0.005);
                schema_.number("ry", "Radius y", 0.01, 2, 0.3, 0.005);
                schema_.number("rotation", "Rotation", -180, 180, 0, 1);
                schema_.number("softness", "Softness", 0, 1, 0.5, 0.01);
            }
            void coverage(const ToolState &state, const Frame &fork, cv::Mat &out, const MaskContext &) const override
            {
                const ParamState &s = as(state);
                cv::Point2d c = fork.project(s.number("cx", 0.5), s.number("cy", 0.5));
                double scale = fork.affineScale();
                double rx = std::max(1e-6, s.number("rx", 0.4) * scale);
                double ry = std::max(1e-6, s.number("ry", 0.3) * scale);
                // The transform's rotation at the centre adds to the mask's
                // own (the local linear map; exact for an affine, CHAIN-7f).
                cv::Matx22d j = fork.jacobian(s.number("cx", 0.5), s.number("cy", 0.5));
                double affineRot = std::atan2(j(1, 0), j(0, 0));
                double a = s.number("rotation") * kPi / 180.0 + affineRot;
                double ca = std::cos(a), sa = std::sin(a);
                double soft = std::clamp(s.number("softness", 0.5), 0.0, 1.0);
                out = cv::Mat(fork.height(), fork.width(), CV_32FC1);
                for (int y = 0; y < fork.height(); ++y)
                {
                    float *row = out.ptr<float>(y);
                    for (int x = 0; x < fork.width(); ++x)
                    {
                        double dx = x + 0.5 - c.x, dy = y + 0.5 - c.y;
                        double u = (dx * ca + dy * sa) / rx;
                        double v = (-dx * sa + dy * ca) / ry;
                        double r = std::sqrt(u * u + v * v);
                        row[x] = static_cast<float>(1.0 - MaskMath::smoothstep(1.0 - soft, 1.0, r));
                    }
                }
            }
        };

        // CHAIN-7c: painted strokes in base space. Each stroke is a
        // polyline of radius r (fraction of the base's shorter side), add
        // or erase (`e`), applied in order: consecutive strokes of one kind
        // are rasterised together (anti-aliased lines and round caps on an
        // 8-bit sheet), softened by a Gaussian of softness × radius, then
        // united with (add) or cut out of (erase) the coverage so far.
        class BrushMask final : public MaskSource
        {
        public:
            BrushMask()
            {
                descriptor_.id = "brush";
                descriptor_.label = "Brush";
                descriptor_.category = ToolCategory::Mask;
                descriptor_.icon = "paintbrush.pointed";
                descriptor_.summary = "{strokes}";
                schema_.strokes("strokes", "Strokes");
                schema_.number("softness", "Softness", 0, 1, 0.25, 0.01);
            }
            void coverage(const ToolState &state, const Frame &fork, cv::Mat &out, const MaskContext &) const override
            {
                const ParamState &s = as(state);
                const nlohmann::json &strokes = s.array("strokes");
                const double softness = std::clamp(s.number("softness", 0.25), 0.0, 1.0);
                const double scale = fork.affineScale();
                out = cv::Mat::zeros(fork.height(), fork.width(), CV_32FC1);
                if (!strokes.is_array() || strokes.empty())
                    return;
                // Runs of consecutive strokes of one kind and similar radius.
                size_t i = 0;
                while (i < strokes.size())
                {
                    const bool erase = strokes[i].value("e", false);
                    const double r0 = strokes[i].value("r", 0.03);
                    cv::Mat sheet = cv::Mat::zeros(fork.height(), fork.width(), CV_8UC1);
                    double radius_px_sum = 0;
                    int n = 0;
                    while (i < strokes.size() && strokes[i].value("e", false) == erase &&
                           std::fabs(strokes[i].value("r", 0.03) - r0) <= 0.2 * r0)
                    {
                        const nlohmann::json &st = strokes[i++];
                        double radius_px = std::max(0.5, st.value("r", 0.03) * scale);
                        radius_px_sum += radius_px;
                        ++n;
                        std::vector<cv::Point> pts;
                        for (const auto &pt : st["p"])
                            if (pt.is_array() && pt.size() == 2)
                            {
                                cv::Point2d q = fork.project(pt[0].get<double>(), pt[1].get<double>());
                                pts.emplace_back(static_cast<int>(std::lround(q.x)), static_cast<int>(std::lround(q.y)));
                            }
                        if (pts.empty())
                            continue;
                        int thickness = std::max(1, static_cast<int>(std::lround(2 * radius_px)));
                        int rad = std::max(1, static_cast<int>(std::lround(radius_px)));
                        cv::circle(sheet, pts.front(), rad, cv::Scalar(255), cv::FILLED, cv::LINE_AA);
                        for (size_t k = 1; k < pts.size(); ++k)
                        {
                            cv::line(sheet, pts[k - 1], pts[k], cv::Scalar(255), thickness, cv::LINE_AA);
                            cv::circle(sheet, pts[k], rad, cv::Scalar(255), cv::FILLED, cv::LINE_AA);
                        }
                    }
                    cv::Mat soft;
                    sheet.convertTo(soft, CV_32FC1, 1.0 / 255.0);
                    double sigma = n > 0 ? softness * (radius_px_sum / n) : 0;
                    if (sigma > 0.3)
                        soft = ChainEngine::gaussianBlur(soft, sigma);
                    if (erase)
                        out = out.mul(1.0f - soft);
                    else
                        cv::max(out, soft, out);
                }
                cv::min(out, 1.0f, out);
                cv::max(out, 0.0f, out);
            }
        };

        // Luma within [lo, hi], soft knees of `knee`.
        class LuminanceMask final : public MaskSource
        {
        public:
            LuminanceMask()
            {
                descriptor_.id = "luminance";
                descriptor_.label = "Luminance range";
                descriptor_.category = ToolCategory::Mask;
                descriptor_.icon = "circle.lefthalf.striped.horizontal.inverse";
                descriptor_.summary = "luminance";
                schema_.number("lo", "Darkest", 0, 1, 0.5, 0.01);
                schema_.number("hi", "Brightest", 0, 1, 1.0, 0.01);
                schema_.number("knee", "Softness", 0, 0.5, 0.1, 0.01);
            }
            void coverage(const ToolState &state, const Frame &fork, cv::Mat &out, const MaskContext &) const override
            {
                const ParamState &s = as(state);
                double lo = s.number("lo", 0.5), hi = s.number("hi", 1.0), knee = s.number("knee", 0.1);
                out = cv::Mat(fork.height(), fork.width(), CV_32FC1);
                for (int y = 0; y < fork.height(); ++y)
                {
                    const cv::Vec3f *px = fork.pixels.ptr<cv::Vec3f>(y);
                    float *row = out.ptr<float>(y);
                    for (int x = 0; x < fork.width(); ++x)
                    {
                        double l = kLumB * px[x][0] + kLumG * px[x][1] + kLumR * px[x][2];
                        double c = MaskMath::smoothstep(lo - knee, lo, l) * (1.0 - MaskMath::smoothstep(hi, hi + knee, l));
                        row[x] = static_cast<float>(c);
                    }
                }
            }
        };

        // Hues within `range` degrees of `hue`, saturated at least `sat_min`.
        class ColorRangeMask final : public MaskSource
        {
        public:
            ColorRangeMask()
            {
                descriptor_.id = "color_range";
                descriptor_.label = "Colour range";
                descriptor_.category = ToolCategory::Mask;
                descriptor_.icon = "paintpalette";
                descriptor_.summary = "colour";
                schema_.number("hue", "Hue", 0, 360, 210, 1);
                schema_.number("range", "Hue range", 1, 180, 30, 1);
                schema_.number("sat_min", "Minimum saturation", 0, 1, 0.15, 0.01);
                schema_.number("softness", "Softness", 0, 1, 0.5, 0.01);
            }
            void coverage(const ToolState &state, const Frame &fork, cv::Mat &out, const MaskContext &) const override
            {
                const ParamState &s = as(state);
                double hue = s.number("hue", 210), range = s.number("range", 30);
                double sat_min = s.number("sat_min", 0.15), soft = s.number("softness", 0.5);
                cv::Mat hsv;
                cv::cvtColor(fork.pixels, hsv, cv::COLOR_BGR2HSV); // H in [0,360), S,V in [0,1] for float input
                out = cv::Mat(fork.height(), fork.width(), CV_32FC1);
                for (int y = 0; y < fork.height(); ++y)
                {
                    const cv::Vec3f *px = hsv.ptr<cv::Vec3f>(y);
                    float *row = out.ptr<float>(y);
                    for (int x = 0; x < fork.width(); ++x)
                    {
                        double d = std::fabs(px[x][0] - hue);
                        d = std::min(d, 360.0 - d);
                        double h = 1.0 - MaskMath::smoothstep(range * (1.0 - soft), range, d);
                        double sat = MaskMath::smoothstep(std::max(0.0, sat_min - 0.1), sat_min, px[x][1]);
                        row[x] = static_cast<float>(h * sat);
                    }
                }
            }
        };
    }

    // ---------------------------------------------------------------- registry

    const MaskRegistry &MaskRegistry::instance()
    {
        static const MaskRegistry r;
        return r;
    }

    namespace
    {
        // ------------------------------------------------ detected (CHAIN-5)

        // A source whose coverage the MaskService computes once per file on
        // the base (docs §5.6). The cached base-space mask is carried to the
        // fork by the frame's affine, then shaped by threshold/softness —
        // view parameters that never re-run a detector, so they are not
        // part of the cache key.
        class DetectedMask : public MaskSource
        {
        public:
            bool detected() const override { return true; }

            void coverage(const ToolState &state, const Frame &fork, cv::Mat &out, const MaskContext &ctx) const override
            {
                const ParamState &s = as(state);
                cv::Mat base_mask;
                MaskService::Status status = MaskService::Status::Unavailable;
                if (!ctx.file_path.empty())
                    status = MaskService::instance().coverage(ctx.file_path, id(), detectorParams(s), ctx.inline_detection, base_mask);
                if (status == MaskService::Status::Pending)
                    ctx.pending = true;
                if (status != MaskService::Status::Ready || base_mask.empty())
                {
                    out = cv::Mat(fork.height(), fork.width(), CV_32FC1, cv::Scalar(0.0f));
                    return;
                }
                // Threshold and softness: smoothstep about the threshold.
                double th = s.number("threshold", 0.5);
                double soft = s.number("softness", 0.15);
                cv::Mat shaped(base_mask.size(), CV_32FC1);
                for (int y = 0; y < base_mask.rows; ++y)
                {
                    const float *src = base_mask.ptr<float>(y);
                    float *dst = shaped.ptr<float>(y);
                    for (int x = 0; x < base_mask.cols; ++x)
                        dst[x] = static_cast<float>(MaskMath::smoothstep(th - soft, th + soft, src[x]));
                }
                transport(shaped, fork, out);
            }

            // Mask pixels → fork pixels: normalise by the mask size, then the
            // frame's base→canvas transform (docs §3.4); a perspective
            // warp when the transform is projective (CHAIN-7f).
            static void transport(const cv::Mat &base_mask, const Frame &fork, cv::Mat &out)
            {
                const cv::Matx33d &m = fork.base_to_canvas;
                double sx = 1.0 / base_mask.cols, sy = 1.0 / base_mask.rows;
                cv::Matx33d h = m * cv::Matx33d(sx, 0, 0, 0, sy, 0, 0, 0, 1);
                if (fork.isAffine())
                {
                    cv::Matx23d a(h(0, 0), h(0, 1), h(0, 2), h(1, 0), h(1, 1), h(1, 2));
                    cv::warpAffine(base_mask, out, a, cv::Size(fork.width(), fork.height()), cv::INTER_LINEAR,
                                   cv::BORDER_CONSTANT, cv::Scalar(0.0f));
                }
                else
                {
                    cv::warpPerspective(base_mask, out, cv::Mat(h), cv::Size(fork.width(), fork.height()), cv::INTER_LINEAR,
                                        cv::BORDER_CONSTANT, cv::Scalar(0.0f));
                }
            }

        protected:
            // The params the detector itself depends on (the cache key).
            virtual nlohmann::json detectorParams(const ParamState &) const { return nlohmann::json::object(); }

            void shapeParams()
            {
                schema_.number("threshold", "Threshold", 0, 1, 0.5, 0.01);
                schema_.number("softness", "Softness", 0, 0.5, 0.15, 0.01);
            }

            // CHAIN-5b: SAM sharpening of a model's coarse coverage. Part of
            // the detector params (off = a different mask), ignored when the
            // model is not installed.
            void refineParam()
            {
                schema_.flag("refine", "Sharpen edges (SAM)", true);
            }
            static nlohmann::json withRefine(nlohmann::json j, const ParamState &s)
            {
                j["refine"] = s.number("refine", 1) != 0;
                return j;
            }
        };

        class SubjectMask final : public DetectedMask
        {
        public:
            SubjectMask()
            {
                descriptor_.id = "subject";
                descriptor_.label = "Subject";
                descriptor_.category = ToolCategory::Mask;
                descriptor_.icon = "person.crop.rectangle";
                descriptor_.summary = "subject";
                refineParam();
                shapeParams();
            }

        protected:
            nlohmann::json detectorParams(const ParamState &s) const override
            {
                return withRefine(nlohmann::json::object(), s);
            }
        };

        class PeopleMask final : public DetectedMask
        {
        public:
            PeopleMask()
            {
                descriptor_.id = "people";
                descriptor_.label = "People";
                descriptor_.category = ToolCategory::Mask;
                descriptor_.icon = "person.2";
                descriptor_.summary = "people";
                refineParam();
                shapeParams();
            }

        protected:
            nlohmann::json detectorParams(const ParamState &s) const override
            {
                return withRefine(nlohmann::json::object(), s);
            }
        };

        class FacesMask final : public DetectedMask
        {
        public:
            FacesMask()
            {
                descriptor_.id = "faces";
                descriptor_.label = "Faces";
                descriptor_.category = ToolCategory::Mask;
                descriptor_.icon = "face.smiling";
                descriptor_.summary = "faces";
                schema_.text("person_ids", "People", "");
                schema_.number("grow", "Grow", 0.5, 3, 1.4, 0.05);
                shapeParams();
            }

        protected:
            nlohmann::json detectorParams(const ParamState &s) const override
            {
                return nlohmann::json{{"person_ids", s.string("person_ids")}, {"grow", s.number("grow", 1.4)}};
            }
        };

        class PromptMask final : public DetectedMask
        {
        public:
            PromptMask()
            {
                descriptor_.id = "prompt";
                descriptor_.label = "Prompt";
                descriptor_.category = ToolCategory::Mask;
                descriptor_.icon = "text.magnifyingglass";
                descriptor_.summary = "{text}";
                schema_.text("text", "Describe it", "");
                refineParam();
                shapeParams();
            }

        protected:
            nlohmann::json detectorParams(const ParamState &s) const override
            {
                // Lower-cased, single-spaced: "the sky" and "the  Sky" share a mask.
                std::string text = s.string("text");
                std::string norm;
                bool space = false;
                for (unsigned char c : text)
                {
                    if (std::isspace(c))
                    {
                        space = !norm.empty();
                        continue;
                    }
                    if (space)
                        norm += ' ';
                    space = false;
                    norm += static_cast<char>(std::tolower(c));
                }
                return withRefine(nlohmann::json{{"text", norm}}, s);
            }
        };
    }

    MaskRegistry::MaskRegistry()
    {
        owned_.push_back(std::make_unique<FullMask>());
        owned_.push_back(std::make_unique<LinearMask>());
        owned_.push_back(std::make_unique<RadialMask>());
        owned_.push_back(std::make_unique<BrushMask>());
        owned_.push_back(std::make_unique<LuminanceMask>());
        owned_.push_back(std::make_unique<ColorRangeMask>());
        owned_.push_back(std::make_unique<SubjectMask>());
        owned_.push_back(std::make_unique<PeopleMask>());
        owned_.push_back(std::make_unique<FacesMask>());
        owned_.push_back(std::make_unique<PromptMask>());
        for (const auto &m : owned_)
            order_.push_back(m.get());
    }

    const MaskSource *MaskRegistry::find(const std::string &id) const
    {
        for (const MaskSource *m : order_)
            if (m->id() == id)
                return m;
        return nullptr;
    }

    std::vector<ToolDescriptor> MaskRegistry::descriptors() const
    {
        std::vector<ToolDescriptor> out;
        for (const MaskSource *m : order_)
            out.push_back(m->descriptor());
        return out;
    }

    // --------------------------------------------------------------- LaneMask

    LaneMask::LaneMask(const LaneMask &other) : invert(other.invert), feather(other.feather)
    {
        for (const Component &c : other.components)
            components.push_back({c.source, c.state ? c.state->clone() : nullptr, c.op});
    }

    LaneMask &LaneMask::operator=(const LaneMask &other)
    {
        if (this != &other)
        {
            components.clear();
            for (const Component &c : other.components)
                components.push_back({c.source, c.state ? c.state->clone() : nullptr, c.op});
            invert = other.invert;
            feather = other.feather;
        }
        return *this;
    }

    bool LaneMask::parse(const nlohmann::json &mask, LaneMask &out, std::string &error)
    {
        LaneMask m;
        if (mask.is_null())
        {
            // No mask = the whole image.
            const MaskSource *full = MaskRegistry::instance().find("full");
            m.components.push_back({full, full->defaultState(), "add"});
            out = std::move(m);
            return true;
        }
        if (!mask.is_object())
        {
            error = "lane mask is not an object";
            return false;
        }
        if (mask.contains("invert"))
        {
            if (!mask["invert"].is_boolean())
            {
                error = "mask invert must be a boolean";
                return false;
            }
            m.invert = mask["invert"].get<bool>();
        }
        if (mask.contains("feather"))
        {
            if (!mask["feather"].is_number() || mask["feather"].get<double>() < 0 || mask["feather"].get<double>() > 0.5)
            {
                error = "mask feather must be within [0, 0.5]";
                return false;
            }
            m.feather = mask["feather"].get<double>();
        }
        nlohmann::json components = mask.value("components", nlohmann::json::array());
        if (!components.is_array())
        {
            error = "mask components is not an array";
            return false;
        }
        for (const auto &c : components)
        {
            if (!c.is_object() || !c.contains("source") || !c["source"].is_string())
            {
                error = "mask component has no source";
                return false;
            }
            std::string id = c["source"].get<std::string>();
            const MaskSource *src = MaskRegistry::instance().find(id);
            if (!src)
            {
                error = "unknown mask source '" + id + "'";
                return false;
            }
            std::string op = c.value("op", std::string("add"));
            if (op != "add" && op != "subtract" && op != "intersect")
            {
                error = "mask component op must be add, subtract or intersect";
                return false;
            }
            std::string perr;
            auto state = src->parse(c.value("params", nlohmann::json::object()), perr);
            if (!state)
            {
                error = "mask " + perr;
                return false;
            }
            m.components.push_back({src, std::move(state), op});
        }
        if (m.components.empty())
        {
            const MaskSource *full = MaskRegistry::instance().find("full");
            m.components.push_back({full, full->defaultState(), "add"});
        }
        out = std::move(m);
        return true;
    }

    bool LaneMask::hasDetected() const
    {
        for (const Component &c : components)
            if (c.source && c.source->detected())
                return true;
        return false;
    }

    void LaneMask::coverage(const Frame &fork, cv::Mat &out, const MaskContext &ctx) const
    {
        cv::Mat acc;
        bool first = true;
        for (const Component &c : components)
        {
            cv::Mat cov;
            c.source->coverage(*c.state, fork, cov, ctx);
            if (first)
            {
                acc = cov;
                first = false;
                continue;
            }
            if (c.op == "add")
                cv::max(acc, cov, acc);
            else if (c.op == "subtract")
                acc = acc.mul(1.0f - cov);
            else
                acc = acc.mul(cov);
        }
        if (acc.empty())
            acc = cv::Mat(fork.height(), fork.width(), CV_32FC1, cv::Scalar(1.0f));
        if (invert)
            acc = 1.0f - acc;
        if (feather > 0)
        {
            double sigma = feather * fork.affineScale();
            if (sigma > 0.3)
                acc = ChainEngine::gaussianBlur(acc, sigma);
        }
        cv::min(acc, 1.0f, acc);
        cv::max(acc, 0.0f, acc);
        out = acc;
    }
}
