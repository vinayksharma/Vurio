#include "adjustments/drivers.hpp"
#include "adjustments/adjustment_document.hpp"
#include <algorithm>
#include <cmath>
#include <functional>

namespace MediaDedup::Adjustments::Drivers
{
    namespace
    {
        using json = nlohmann::json;

        struct Target
        {
            double min = 0, max = 1;
            bool integer = false;
        };

        // The limits of `target` on a tool node; false when it cannot be driven.
        bool toolTarget(const AdjustmentTool &tool, const std::string &target, Target &out, std::string &error)
        {
            if (target == kStrength)
            {
                if (tool.category() == ToolCategory::Geometry)
                {
                    error = "a geometry tool has no strength to drive";
                    return false;
                }
                out = Target{0, 1, false};
                return true;
            }
            for (const ParamSpec &p : tool.descriptor().params)
            {
                if (p.id != target)
                    continue;
                if (p.kind != ParamSpec::Kind::Number && p.kind != ParamSpec::Kind::Integer)
                {
                    error = "'" + target + "' is not a number and cannot be driven";
                    return false;
                }
                out = Target{p.min, p.max, p.kind == ParamSpec::Kind::Integer};
                return true;
            }
            error = "the tool has no parameter '" + target + "'";
            return false;
        }

        bool validateDriver(const json &d, const Target &t, std::string &error)
        {
            if (!d.is_object() || !d.contains("signal") || !d["signal"].is_string())
            {
                error = "a driver needs a signal";
                return false;
            }
            if (!SceneSignals::find(d["signal"].get<std::string>()))
            {
                error = "unknown signal '" + d["signal"].get<std::string>() + "'";
                return false;
            }
            if (!d.contains("points") || !d["points"].is_array() || d["points"].empty() ||
                d["points"].size() > static_cast<size_t>(kMaxPoints))
            {
                error = "a driver needs 1 to " + std::to_string(kMaxPoints) + " points";
                return false;
            }
            double last = 0;
            bool first = true;
            for (const auto &pt : d["points"])
            {
                if (!pt.is_array() || pt.size() != 2 || !pt[0].is_number() || !pt[1].is_number())
                {
                    error = "a driver point is [signal value, value]";
                    return false;
                }
                double x = pt[0].get<double>(), y = pt[1].get<double>();
                if (!std::isfinite(x) || !std::isfinite(y) || (!first && x <= last))
                {
                    error = "driver points must have increasing signal values";
                    return false;
                }
                if (y < t.min - 1e-9 || y > t.max + 1e-9)
                {
                    error = "a driver value is outside the limits of what it drives";
                    return false;
                }
                last = x;
                first = false;
            }
            return true;
        }

        // Visits every (owner, tool-or-null) that may carry drivers: tool
        // nodes with their tool, lanes with null. Stops on false.
        using Visitor = std::function<bool(json &owner, const AdjustmentTool *tool, bool is_lane)>;
        bool walk(json &chain, const ToolRegistry &registry, const Visitor &visit)
        {
            if (!chain.is_array())
                return true;
            for (auto &node : chain)
            {
                if (!node.is_object())
                    continue;
                std::string tool_id = node.value("tool", std::string());
                if (tool_id == AdjustmentDocument::kSplitTool)
                {
                    if (!node.contains("lanes") || !node["lanes"].is_array())
                        continue;
                    for (auto &lane : node["lanes"])
                    {
                        if (!lane.is_object())
                            continue;
                        if (!visit(lane, nullptr, true))
                            return false;
                        if (lane.contains("steps") && !walk(lane["steps"], registry, visit))
                            return false;
                    }
                    continue;
                }
                if (!visit(node, registry.find(tool_id), false))
                    return false;
            }
            return true;
        }

        double rounded(double v) { return std::round(v * 1e6) / 1e6; }
    }

    bool validate(const nlohmann::json &chain_in, const ToolRegistry &registry, std::string &error)
    {
        json chain = chain_in; // walk wants a mutable tree; nothing is changed
        return walk(chain, registry, [&](json &owner, const AdjustmentTool *tool, bool is_lane)
        {
            if (!owner.contains(kKey))
                return true;
            const json &drivers = owner[kKey];
            std::string where = (is_lane ? "lane " : "node=") + owner.value("id", std::string()) + " ";
            if (!drivers.is_object())
            {
                error = where + "drivers is not an object";
                return false;
            }
            for (auto it = drivers.begin(); it != drivers.end(); ++it)
            {
                Target t;
                std::string why;
                if (is_lane)
                {
                    if (it.key() != kStrength)
                    {
                        error = where + "a lane can only drive its strength";
                        return false;
                    }
                    t = Target{0, 1, false};
                }
                else if (!tool)
                    continue; // unknown tool: kept, never resolved
                else if (!toolTarget(*tool, it.key(), t, why))
                {
                    error = where + why;
                    return false;
                }
                if (!validateDriver(it.value(), t, why))
                {
                    error = where + "driver '" + it.key() + "': " + why;
                    return false;
                }
            }
            return true;
        });
    }

    bool evaluate(const nlohmann::json &driver, const SceneSignals &signals, double &out)
    {
        if (!driver.is_object() || !driver.contains("signal") || !driver["signal"].is_string() ||
            !driver.contains("points") || !driver["points"].is_array() || driver["points"].empty())
            return false;
        const std::string signal = driver["signal"].get<std::string>();
        if (!signals.has(signal))
            return false;
        const double x = signals.value(signal);
        const json &pts = driver["points"];
        auto px = [&](size_t i) { return pts[i][0].get<double>(); };
        auto py = [&](size_t i) { return pts[i][1].get<double>(); };
        for (const auto &pt : pts)
            if (!pt.is_array() || pt.size() != 2 || !pt[0].is_number() || !pt[1].is_number())
                return false;
        if (x <= px(0))
        {
            out = py(0);
            return true;
        }
        for (size_t i = 1; i < pts.size(); ++i)
            if (x <= px(i))
            {
                double span = px(i) - px(i - 1);
                double f = span > 0 ? (x - px(i - 1)) / span : 1.0;
                out = py(i - 1) + (py(i) - py(i - 1)) * f;
                return true;
            }
        out = py(pts.size() - 1);
        return true;
    }

    int resolve(nlohmann::json &chain, const SceneSignals &signals, const ToolRegistry &registry)
    {
        int count = 0;
        walk(chain, registry, [&](json &owner, const AdjustmentTool *tool, bool is_lane)
        {
            if (!owner.contains(kKey) || !owner[kKey].is_object())
                return true;
            const json drivers = owner[kKey];
            for (auto it = drivers.begin(); it != drivers.end(); ++it)
            {
                double v = 0;
                if (!evaluate(it.value(), signals, v))
                    continue;
                if (is_lane)
                {
                    if (it.key() != kStrength)
                        continue;
                    if (!owner.contains("merge") || !owner["merge"].is_object())
                        owner["merge"] = json::object();
                    owner["merge"]["strength"] = rounded(std::min(1.0, std::max(0.0, v)));
                    ++count;
                    continue;
                }
                Target t;
                std::string why;
                if (!tool || !toolTarget(*tool, it.key(), t, why))
                    continue;
                v = std::min(t.max, std::max(t.min, v));
                if (it.key() == kStrength)
                {
                    owner["strength"] = rounded(v);
                    ++count;
                    continue;
                }
                if (!owner.contains("params") || !owner["params"].is_object())
                    owner["params"] = json::object();
                if (t.integer)
                    owner["params"][it.key()] = static_cast<int>(std::lround(v));
                else
                    owner["params"][it.key()] = rounded(v);
                ++count;
            }
            return true;
        });
        return count;
    }

    bool hasAny(const nlohmann::json &chain) { return !canonical(chain).empty(); }

    std::string canonical(const nlohmann::json &chain_in)
    {
        json chain = chain_in;
        json out = json::array();
        int position = 0;
        bool any = false;
        // Positions count every visited owner, so the same driver on a
        // different node is a different recipe.
        walk(chain, ToolRegistry::instance(), [&](json &owner, const AdjustmentTool *, bool)
        {
            ++position;
            if (!owner.contains(kKey) || !owner[kKey].is_object() || owner[kKey].empty())
                return true;
            json entry = json::object();
            for (auto it = owner[kKey].begin(); it != owner[kKey].end(); ++it)
            {
                json d = json::object();
                d["signal"] = it.value().value("signal", std::string());
                json pts = json::array();
                if (it.value().contains("points") && it.value()["points"].is_array())
                    for (const auto &pt : it.value()["points"])
                        if (pt.is_array() && pt.size() == 2 && pt[0].is_number() && pt[1].is_number())
                            pts.push_back({rounded(pt[0].get<double>()), rounded(pt[1].get<double>())});
                d["points"] = pts;
                entry[it.key()] = d;
            }
            out.push_back({position, entry});
            any = true;
            return true;
        });
        return any ? out.dump() : std::string();
    }
}
