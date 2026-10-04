#include "adjustments/tools/rotate_tool.hpp"
#include <cmath>
#include <iomanip>
#include <sstream>

namespace MediaDedup::Adjustments
{
    namespace
    {
        const RotateState &as(const ToolState &s) { return static_cast<const RotateState &>(s); }

        std::string fixed(double v, int decimals = 4)
        {
            std::ostringstream os;
            os << std::fixed << std::setprecision(decimals) << v;
            return os.str();
        }
    }

    RotateTool::RotateTool()
    {
        descriptor_.id = kId;
        descriptor_.label = "Rotate";
        descriptor_.version = kVersion;

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

    void RotateTool::appendMagickOps(const ToolState &state, std::vector<std::string> &args) const
    {
        const RotateState &s = as(state);
        if (isIdentity(s))
            return;
        if (std::fabs(s.angle) >= 1e-9)
        {
            // The corners -rotate exposes are cropped away by the chain's
            // crop (docs §3.3); black keeps any sub-pixel sliver unobtrusive.
            args.push_back("-background");
            args.push_back("black");
        }
        args.push_back("-rotate");
        args.push_back(fixed(totalDegrees(s)));
        args.push_back("+repage");
    }

    Size RotateTool::transformSize(const ToolState &state, Size input) const
    {
        const RotateState &s = as(state);
        return rotatedCanvas(input, s.quarter_turns, s.angle);
    }
}
