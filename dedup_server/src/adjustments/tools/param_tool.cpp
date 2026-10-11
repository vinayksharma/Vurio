#include "adjustments/tools/param_tool.hpp"
#include <algorithm>
#include <cmath>

namespace MediaDedup::Adjustments
{
    void ParamTool::number(const char *id, const char *label, double min, double max, double def, double step)
    {
        ParamSpec p;
        p.id = id;
        p.label = label;
        p.kind = ParamSpec::Kind::Number;
        p.min = min;
        p.max = max;
        p.def = def;
        p.step = step;
        descriptor_.params.push_back(p);
    }

    void ParamTool::integer(const char *id, const char *label, int min, int max, int def)
    {
        ParamSpec p;
        p.id = id;
        p.label = label;
        p.kind = ParamSpec::Kind::Integer;
        p.min = min;
        p.max = max;
        p.def = def;
        p.step = 1;
        descriptor_.params.push_back(p);
    }

    void ParamTool::choice(const char *id, const char *label, std::vector<std::string> options, const char *def)
    {
        ParamSpec p;
        p.id = id;
        p.label = label;
        p.kind = ParamSpec::Kind::Enum;
        p.options = std::move(options);
        p.default_option = def;
        descriptor_.params.push_back(p);
    }

    void ParamTool::flag(const char *id, const char *label, bool def)
    {
        ParamSpec p;
        p.id = id;
        p.label = label;
        p.kind = ParamSpec::Kind::Bool;
        p.def = def ? 1 : 0;
        descriptor_.params.push_back(p);
    }

    void ParamTool::text(const char *id, const char *label, const char *def)
    {
        ParamSpec p;
        p.id = id;
        p.label = label;
        p.kind = ParamSpec::Kind::Text;
        p.default_option = def;
        descriptor_.params.push_back(p);
    }

    std::unique_ptr<ToolState> ParamTool::defaultState() const
    {
        auto s = std::make_unique<ParamState>();
        for (const auto &p : descriptor_.params)
        {
            if (p.kind == ParamSpec::Kind::Enum || p.kind == ParamSpec::Kind::Text)
                s->strings[p.id] = p.default_option;
            else
                s->numbers[p.id] = p.def;
        }
        return s;
    }

    std::unique_ptr<ToolState> ParamTool::parse(const nlohmann::json &params, std::string &error) const
    {
        const std::string &id = descriptor_.id;
        if (!params.is_object())
        {
            error = id + ": params is not an object";
            return nullptr;
        }
        int version = params.value("version", 1);
        if (version > descriptor_.version)
        {
            error = id + ": params version " + std::to_string(version) + " is newer than supported " +
                    std::to_string(descriptor_.version);
            return nullptr;
        }
        auto state = defaultState();
        auto &s = static_cast<ParamState &>(*state);
        for (const auto &p : descriptor_.params)
        {
            if (!params.contains(p.id))
                continue;
            const nlohmann::json &v = params[p.id];
            switch (p.kind)
            {
            case ParamSpec::Kind::Number:
            case ParamSpec::Kind::Integer:
            {
                if (!v.is_number() || (p.kind == ParamSpec::Kind::Integer && !v.is_number_integer()))
                {
                    error = id + ": " + p.id + (p.kind == ParamSpec::Kind::Integer ? " must be an integer" : " must be a number");
                    return nullptr;
                }
                double d = v.get<double>();
                if (!std::isfinite(d) || d < p.min - 1e-9 || d > p.max + 1e-9)
                {
                    error = id + ": " + p.id + " must be within [" + std::to_string(p.min) + ", " + std::to_string(p.max) + "]";
                    return nullptr;
                }
                s.numbers[p.id] = d;
                break;
            }
            case ParamSpec::Kind::Bool:
                if (!v.is_boolean())
                {
                    error = id + ": " + p.id + " must be a boolean";
                    return nullptr;
                }
                s.numbers[p.id] = v.get<bool>() ? 1 : 0;
                break;
            case ParamSpec::Kind::Enum:
            {
                if (!v.is_string())
                {
                    error = id + ": " + p.id + " must be a string";
                    return nullptr;
                }
                std::string o = v.get<std::string>();
                if (std::find(p.options.begin(), p.options.end(), o) == p.options.end())
                {
                    error = id + ": unknown " + p.id + " '" + o + "'";
                    return nullptr;
                }
                s.strings[p.id] = o;
                break;
            }
            case ParamSpec::Kind::Text:
                if (!v.is_string())
                {
                    error = id + ": " + p.id + " must be a string";
                    return nullptr;
                }
                s.strings[p.id] = v.get<std::string>();
                break;
            }
        }
        return state;
    }

    nlohmann::json ParamTool::serialize(const ToolState &state) const
    {
        const ParamState &s = as(state);
        nlohmann::json j;
        j["version"] = descriptor_.version;
        for (const auto &p : descriptor_.params)
        {
            switch (p.kind)
            {
            case ParamSpec::Kind::Number: j[p.id] = s.number(p.id, p.def); break;
            case ParamSpec::Kind::Integer: j[p.id] = static_cast<int>(std::lround(s.number(p.id, p.def))); break;
            case ParamSpec::Kind::Bool: j[p.id] = s.number(p.id, p.def) != 0; break;
            case ParamSpec::Kind::Enum:
            case ParamSpec::Kind::Text: j[p.id] = s.strings.count(p.id) ? s.string(p.id) : p.default_option; break;
            }
        }
        return j;
    }

    bool ParamTool::isIdentity(const ToolState &state) const
    {
        const ParamState &s = as(state);
        for (const auto &p : descriptor_.params)
        {
            if (p.kind == ParamSpec::Kind::Enum || p.kind == ParamSpec::Kind::Text)
            {
                if (s.strings.count(p.id) && s.string(p.id) != p.default_option)
                    return false;
            }
            else if (std::fabs(s.number(p.id, p.def) - p.def) > 1e-9)
                return false;
        }
        return true;
    }
}
