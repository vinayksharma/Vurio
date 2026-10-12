#include "adjustments/tools/param_tool.hpp"
#include <algorithm>
#include <cmath>

namespace MediaDedup::Adjustments
{
    void ParamSchema::group(const char *name) { group_ = name ? name : ""; }

    void ParamSchema::curve(const char *id, const char *label)
    {
        ParamSpec p;
        p.id = id;
        p.label = label;
        p.kind = ParamSpec::Kind::Curve;
        p.default_json = CurvePoints::identityJson().dump();
        p.group = group_;
        descriptor_.params.push_back(p);
    }

    void ParamSchema::strokes(const char *id, const char *label)
    {
        ParamSpec p;
        p.id = id;
        p.label = label;
        p.kind = ParamSpec::Kind::Strokes;
        p.default_json = "[]";
        p.group = group_;
        descriptor_.params.push_back(p);
    }

    bool ParamSchema::validateCurve(const nlohmann::json &v, std::string &error)
    {
        if (!v.is_array() || v.size() < 2 || v.size() > static_cast<size_t>(ParamSpec::kCurveMaxPoints))
        {
            error = "must be an array of 2 to " + std::to_string(ParamSpec::kCurveMaxPoints) + " points";
            return false;
        }
        double last_x = -1;
        for (const auto &pt : v)
        {
            if (!pt.is_array() || pt.size() != 2 || !pt[0].is_number() || !pt[1].is_number())
            {
                error = "every point must be [x, y]";
                return false;
            }
            double x = pt[0].get<double>(), y = pt[1].get<double>();
            if (!std::isfinite(x) || !std::isfinite(y) || x < -1e-9 || x > 1 + 1e-9 || y < -1e-9 || y > 1 + 1e-9)
            {
                error = "points must lie within [0, 1]";
                return false;
            }
            if (x <= last_x + 1e-6)
            {
                error = "point x values must be strictly increasing";
                return false;
            }
            last_x = x;
        }
        return true;
    }

    bool ParamSchema::validateStrokes(const nlohmann::json &v, std::string &error)
    {
        if (!v.is_array() || v.size() > static_cast<size_t>(ParamSpec::kStrokesMax))
        {
            error = "must be an array of at most " + std::to_string(ParamSpec::kStrokesMax) + " strokes";
            return false;
        }
        for (const auto &st : v)
        {
            if (!st.is_object() || !st.contains("r") || !st["r"].is_number() || !st.contains("p") || !st["p"].is_array())
            {
                error = "every stroke must be {r, p: [[x, y], …]} with an optional e";
                return false;
            }
            double r = st["r"].get<double>();
            if (!std::isfinite(r) || r < 0.0005 || r > 0.5)
            {
                error = "stroke radius must be within [0.0005, 0.5] of the short side";
                return false;
            }
            if (st.contains("e") && !st["e"].is_boolean())
            {
                error = "stroke e (erase) must be a boolean";
                return false;
            }
            const auto &pts = st["p"];
            if (pts.empty() || pts.size() > static_cast<size_t>(ParamSpec::kStrokePointsMax))
            {
                error = "a stroke needs 1 to " + std::to_string(ParamSpec::kStrokePointsMax) + " points";
                return false;
            }
            for (const auto &pt : pts)
            {
                if (!pt.is_array() || pt.size() != 2 || !pt[0].is_number() || !pt[1].is_number())
                {
                    error = "every stroke point must be [x, y]";
                    return false;
                }
                double x = pt[0].get<double>(), y = pt[1].get<double>();
                if (!std::isfinite(x) || !std::isfinite(y) || x < -0.5 || x > 1.5 || y < -0.5 || y > 1.5)
                {
                    error = "stroke points must lie within [-0.5, 1.5] of the base";
                    return false;
                }
            }
        }
        return true;
    }

    // ------------------------------------------------------------- curves

    CurvePoints CurvePoints::fromJson(const nlohmann::json &value)
    {
        CurvePoints c;
        if (value.is_array())
            for (const auto &pt : value)
                if (pt.is_array() && pt.size() == 2 && pt[0].is_number() && pt[1].is_number())
                    c.points.emplace_back(pt[0].get<double>(), pt[1].get<double>());
        std::sort(c.points.begin(), c.points.end());
        if (c.points.size() < 2)
            c.points = {{0.0, 0.0}, {1.0, 1.0}};
        return c;
    }

    bool CurvePoints::isIdentity() const
    {
        for (const auto &p : points)
            if (std::fabs(p.first - p.second) > 1e-9)
                return false;
        return points.size() >= 2 && std::fabs(points.front().first) < 1e-9 && std::fabs(points.back().first - 1) < 1e-9;
    }

    double CurvePoints::evaluate(double x) const
    {
        const size_t n = points.size();
        if (n == 0)
            return x;
        if (x <= points.front().first)
            return points.front().second;
        if (x >= points.back().first)
            return points.back().second;
        // Fritsch–Carlson tangents: monotone where the data is monotone.
        std::vector<double> d(n - 1), m(n, 0.0);
        for (size_t i = 0; i + 1 < n; ++i)
        {
            double h = points[i + 1].first - points[i].first;
            d[i] = h > 1e-12 ? (points[i + 1].second - points[i].second) / h : 0.0;
        }
        if (n == 2)
            m[0] = m[1] = d[0];
        else
        {
            m[0] = d[0];
            m[n - 1] = d[n - 2];
            for (size_t i = 1; i + 1 < n; ++i)
            {
                if (d[i - 1] * d[i] <= 0)
                    m[i] = 0;
                else
                {
                    double h0 = points[i].first - points[i - 1].first, h1 = points[i + 1].first - points[i].first;
                    double w1 = 2 * h1 + h0, w2 = h1 + 2 * h0;
                    m[i] = (w1 + w2) / (w1 / d[i - 1] + w2 / d[i]);
                }
            }
        }
        size_t i = 0;
        while (i + 2 < n && x > points[i + 1].first)
            ++i;
        double h = points[i + 1].first - points[i].first;
        if (h <= 1e-12)
            return points[i + 1].second;
        double t = (x - points[i].first) / h;
        double t2 = t * t, t3 = t2 * t;
        double h00 = 2 * t3 - 3 * t2 + 1, h10 = t3 - 2 * t2 + t, h01 = -2 * t3 + 3 * t2, h11 = t3 - t2;
        double y = h00 * points[i].second + h10 * h * m[i] + h01 * points[i + 1].second + h11 * h * m[i + 1];
        return std::min(1.0, std::max(0.0, y));
    }

    void CurvePoints::lut(int size, std::vector<float> &out) const
    {
        out.resize(static_cast<size_t>(std::max(2, size)));
        for (size_t k = 0; k < out.size(); ++k)
            out[k] = static_cast<float>(evaluate(static_cast<double>(k) / (out.size() - 1)));
    }

    void ParamSchema::number(const char *id, const char *label, double min, double max, double def, double step)
    {
        ParamSpec p;
        p.id = id;
        p.label = label;
        p.kind = ParamSpec::Kind::Number;
        p.min = min;
        p.max = max;
        p.def = def;
        p.step = step;
        p.group = group_;
        descriptor_.params.push_back(p);
    }

    void ParamSchema::integer(const char *id, const char *label, int min, int max, int def)
    {
        ParamSpec p;
        p.id = id;
        p.label = label;
        p.kind = ParamSpec::Kind::Integer;
        p.min = min;
        p.max = max;
        p.def = def;
        p.step = 1;
        p.group = group_;
        descriptor_.params.push_back(p);
    }

    void ParamSchema::choice(const char *id, const char *label, std::vector<std::string> options, const char *def)
    {
        ParamSpec p;
        p.id = id;
        p.label = label;
        p.kind = ParamSpec::Kind::Enum;
        p.options = std::move(options);
        p.default_option = def;
        p.group = group_;
        descriptor_.params.push_back(p);
    }

    void ParamSchema::flag(const char *id, const char *label, bool def)
    {
        ParamSpec p;
        p.id = id;
        p.label = label;
        p.kind = ParamSpec::Kind::Bool;
        p.def = def ? 1 : 0;
        p.group = group_;
        descriptor_.params.push_back(p);
    }

    void ParamSchema::text(const char *id, const char *label, const char *def)
    {
        ParamSpec p;
        p.id = id;
        p.label = label;
        p.kind = ParamSpec::Kind::Text;
        p.default_option = def;
        p.group = group_;
        descriptor_.params.push_back(p);
    }

    std::unique_ptr<ToolState> ParamSchema::defaultState() const
    {
        auto s = std::make_unique<ParamState>();
        for (const auto &p : descriptor_.params)
        {
            if (p.kind == ParamSpec::Kind::Enum || p.kind == ParamSpec::Kind::Text)
                s->strings[p.id] = p.default_option;
            else if (p.kind == ParamSpec::Kind::Curve || p.kind == ParamSpec::Kind::Strokes)
                s->arrays[p.id] = nlohmann::json::parse(p.default_json, nullptr, false);
            else
                s->numbers[p.id] = p.def;
        }
        return s;
    }

    std::unique_ptr<ToolState> ParamSchema::parse(const nlohmann::json &params, std::string &error) const
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
            case ParamSpec::Kind::Curve:
            case ParamSpec::Kind::Strokes:
            {
                std::string why;
                bool ok = p.kind == ParamSpec::Kind::Curve ? validateCurve(v, why) : validateStrokes(v, why);
                if (!ok)
                {
                    error = id + ": " + p.id + " " + why;
                    return nullptr;
                }
                s.arrays[p.id] = v;
                break;
            }
            }
        }
        return state;
    }

    nlohmann::json ParamSchema::serialize(const ToolState &state) const
    {
        const ParamState &s = static_cast<const ParamState &>(state);
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
            case ParamSpec::Kind::Curve:
            case ParamSpec::Kind::Strokes:
                j[p.id] = s.arrays.count(p.id) ? s.array(p.id) : nlohmann::json::parse(p.default_json, nullptr, false);
                break;
            }
        }
        return j;
    }

    bool ParamSchema::isIdentity(const ToolState &state) const
    {
        const ParamState &s = static_cast<const ParamState &>(state);
        for (const auto &p : descriptor_.params)
        {
            if (p.kind == ParamSpec::Kind::Enum || p.kind == ParamSpec::Kind::Text)
            {
                if (s.strings.count(p.id) && s.string(p.id) != p.default_option)
                    return false;
            }
            else if (p.kind == ParamSpec::Kind::Curve)
            {
                if (s.arrays.count(p.id) && !CurvePoints::fromJson(s.array(p.id)).isIdentity())
                    return false;
            }
            else if (p.kind == ParamSpec::Kind::Strokes)
            {
                if (s.arrays.count(p.id) && !s.array(p.id).empty())
                    return false;
            }
            else if (std::fabs(s.number(p.id, p.def) - p.def) > 1e-9)
                return false;
        }
        return true;
    }
}
