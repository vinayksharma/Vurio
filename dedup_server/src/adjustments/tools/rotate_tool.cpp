#include "adjustments/tools/rotate_tool.hpp"
#include <cmath>
#include <opencv2/imgproc.hpp>

namespace MediaDedup::Adjustments
{
    namespace
    {
        const RotateState &as(const ToolState &s) { return static_cast<const RotateState &>(s); }
    }

    RotateTool::RotateTool()
    {
        descriptor_.id = kId;
        descriptor_.label = "Rotate";
        descriptor_.version = kVersion;
        descriptor_.category = ToolCategory::Geometry;
        descriptor_.icon = "rotate.right";
        descriptor_.lane_allowed = false;
        descriptor_.summary = "{total_degrees:+.1f}°";

        ParamSpec turns;
        turns.id = "quarter_turns";
        turns.label = "Quarter turns";
        turns.kind = ParamSpec::Kind::Integer;
        turns.min = 0;
        turns.max = 3;
        turns.step = 1;
        turns.def = 0;
        descriptor_.params.push_back(turns);

        ParamSpec angle;
        angle.id = "angle";
        angle.label = "Straighten";
        angle.kind = ParamSpec::Kind::Number;
        angle.min = -kMaxAngle;
        angle.max = kMaxAngle;
        angle.step = 0.1;
        angle.def = 0;
        descriptor_.params.push_back(angle);
    }

    std::unique_ptr<ToolState> RotateTool::defaultState() const
    {
        return std::make_unique<RotateState>();
    }

    std::unique_ptr<ToolState> RotateTool::parse(const nlohmann::json &section, std::string &error) const
    {
        if (!section.is_object())
        {
            error = "rotate: section is not an object";
            return nullptr;
        }
        int version = section.value("version", 1);
        if (version > kVersion)
        {
            error = "rotate: section version " + std::to_string(version) + " is newer than supported " +
                    std::to_string(kVersion);
            return nullptr;
        }
        auto state = std::make_unique<RotateState>();
        if (section.contains("quarter_turns"))
        {
            if (!section["quarter_turns"].is_number_integer())
            {
                error = "rotate: quarter_turns must be an integer";
                return nullptr;
            }
            state->quarter_turns = section["quarter_turns"].get<int>();
            if (state->quarter_turns < 0 || state->quarter_turns > 3)
            {
                error = "rotate: quarter_turns must be 0..3";
                return nullptr;
            }
        }
        if (section.contains("angle"))
        {
            if (!section["angle"].is_number())
            {
                error = "rotate: angle must be a number";
                return nullptr;
            }
            state->angle = section["angle"].get<double>();
            if (!std::isfinite(state->angle) || state->angle < -kMaxAngle || state->angle > kMaxAngle)
            {
                error = "rotate: angle must be within [-45, 45]";
                return nullptr;
            }
        }
        return state;
    }

    nlohmann::json RotateTool::serialize(const ToolState &state) const
    {
        const RotateState &s = as(state);
        nlohmann::json j;
        j["version"] = kVersion;
        j["quarter_turns"] = s.quarter_turns;
        j["angle"] = s.angle;
        return j;
    }

    bool RotateTool::isIdentity(const ToolState &state) const
    {
        const RotateState &s = as(state);
        return s.quarter_turns == 0 && std::fabs(s.angle) < 1e-9;
    }

    bool RotateTool::validate(const ToolState &, Size, std::string &) const
    {
        return true; // every in-range state is renderable
    }

    void RotateTool::apply(const ToolState &state, Frame &io, const RenderContext &) const
    {
        const RotateState &s = as(state);
        if (isIdentity(s) || io.empty())
            return;
        // Exact quarter turns first: lossless, and they decide the canvas
        // the fine angle expands.
        const double w0 = io.width(), h0 = io.height();
        switch (s.quarter_turns)
        {
        case 1:
            cv::rotate(io.pixels, io.pixels, cv::ROTATE_90_CLOCKWISE);
            io.composeAffine(cv::Matx23d(0, -1, h0, 1, 0, 0)); // (x,y) → (h0 − y, x)
            break;
        case 2:
            cv::rotate(io.pixels, io.pixels, cv::ROTATE_180);
            io.composeAffine(cv::Matx23d(-1, 0, w0, 0, -1, h0));
            break;
        case 3:
            cv::rotate(io.pixels, io.pixels, cv::ROTATE_90_COUNTERCLOCKWISE);
            io.composeAffine(cv::Matx23d(0, 1, 0, -1, 0, w0)); // (x,y) → (y, w0 − x)
            break;
        default: break;
        }
        if (std::fabs(s.angle) < 1e-9)
            return;
        // Fine angle about the centre into the canvas the geometry predicts
        // (rotatedBounds: ImageMagick's rule, so display dims, the client's
        // overlay and every stored crop agree). Our angle is clockwise;
        // OpenCV's is counter-clockwise. Black outside the content: the
        // chain's crop removes those corners (docs §3.3).
        Size turned = io.size();
        Size canvas = rotatedBounds(turned, s.angle);
        int cw = static_cast<int>(std::lround(canvas.width));
        int ch = static_cast<int>(std::lround(canvas.height));
        cv::Mat m = cv::getRotationMatrix2D(cv::Point2f(static_cast<float>(turned.width / 2.0),
                                                        static_cast<float>(turned.height / 2.0)),
                                            -s.angle, 1.0);
        m.at<double>(0, 2) += cw / 2.0 - turned.width / 2.0;
        m.at<double>(1, 2) += ch / 2.0 - turned.height / 2.0;
        cv::Mat out;
        cv::warpAffine(io.pixels, out, m, cv::Size(cw, ch), cv::INTER_CUBIC, cv::BORDER_CONSTANT,
                       cv::Scalar(0, 0, 0));
        io.pixels = out;
        io.composeAffine(cv::Matx23d(m.at<double>(0, 0), m.at<double>(0, 1), m.at<double>(0, 2),
                                     m.at<double>(1, 0), m.at<double>(1, 1), m.at<double>(1, 2)));
    }

    Size RotateTool::transformSize(const ToolState &state, Size input) const
    {
        const RotateState &s = as(state);
        return rotatedCanvas(input, s.quarter_turns, s.angle);
    }
}
