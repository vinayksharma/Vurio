#include "adjustments/adjustment_document.hpp"
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <random>
#include <set>
#include <sstream>

namespace MediaDedup::Adjustments
{
    namespace
    {
        // Deep copy with every floating-point number rounded to 6 decimals,
        // so two serializations of the same values cannot differ in the
        // last bits.
        nlohmann::json rounded(const nlohmann::json &j)
        {
            if (j.is_number_float())
            {
                double v = std::round(j.get<double>() * 1e6) / 1e6;
                if (v == 0.0)
                    v = 0.0; // fold -0
                return v;
            }
            if (j.is_object())
            {
                nlohmann::json o = nlohmann::json::object();
                for (auto it = j.begin(); it != j.end(); ++it)
                    o[it.key()] = rounded(it.value());
                return o;
            }
            if (j.is_array())
            {
                nlohmann::json a = nlohmann::json::array();
                for (const auto &v : j)
                    a.push_back(rounded(v));
                return a;
            }
            return j;
        }

        bool nodeEnabled(const nlohmann::json &node)
        {
            return !(node.contains("enabled") && node["enabled"].is_boolean() && !node["enabled"].get<bool>());
        }

        // One node's canonical form: tool + params (+ blend/strength when not
        // the default); a split carries its lanes recursively, each lane its
        // mask, steps and merge. Ids, names and enabled flags never appear.
        nlohmann::json canonicalNode(const nlohmann::json &node);

        nlohmann::json canonicalChainArray(const nlohmann::json &chain)
        {
            nlohmann::json out = nlohmann::json::array();
            if (!chain.is_array())
                return out;
            for (const auto &node : chain)
            {
                if (!node.is_object() || !nodeEnabled(node))
                    continue;
                out.push_back(canonicalNode(node));
            }
            return out;
        }

        nlohmann::json canonicalNode(const nlohmann::json &node)
        {
            nlohmann::json c = nlohmann::json::object();
            c["tool"] = node.value("tool", std::string());
            if (node.contains("params"))
                c["params"] = rounded(node["params"]);
            if (node.contains("blend") && node["blend"].is_string() && node["blend"].get<std::string>() != "normal")
                c["blend"] = node["blend"];
            if (node.contains("strength") && node["strength"].is_number())
            {
                double s = std::round(node["strength"].get<double>() * 1e6) / 1e6;
                if (std::fabs(s - 1.0) > 1e-9)
                    c["strength"] = s;
            }
            if (node.contains("lanes") && node["lanes"].is_array())
            {
                nlohmann::json lanes = nlohmann::json::array();
                for (const auto &lane : node["lanes"])
                {
                    if (!lane.is_object() || !nodeEnabled(lane))
                        continue;
                    nlohmann::json l = nlohmann::json::object();
                    if (lane.contains("mask"))
                        l["mask"] = rounded(lane["mask"]);
                    l["steps"] = canonicalChainArray(lane.value("steps", nlohmann::json::array()));
                    if (lane.contains("merge"))
                        l["merge"] = rounded(lane["merge"]);
                    lanes.push_back(l);
                }
                c["lanes"] = lanes;
            }
            return c;
        }

        bool validateChainShape(const nlohmann::json &chain, const std::string &where, std::set<std::string> &ids,
                                std::string &error);

        bool validateNodeShape(const nlohmann::json &node, size_t index, std::string &error)
        {
            if (!node.is_object())
            {
                error = "chain[" + std::to_string(index) + "] is not an object";
                return false;
            }
            if (node.contains("lanes"))
            {
                if (!node["lanes"].is_array())
                {
                    error = "chain[" + std::to_string(index) + "] lanes is not an array";
                    return false;
                }
                for (const auto &lane : node["lanes"])
                {
                    if (!lane.is_object())
                    {
                        error = "chain[" + std::to_string(index) + "] has a lane that is not an object";
                        return false;
                    }
                    if (lane.contains("steps") && !lane["steps"].is_array())
                    {
                        error = "chain[" + std::to_string(index) + "] lane steps is not an array";
                        return false;
                    }
                    if (lane.contains("mask") && !lane["mask"].is_object() && !lane["mask"].is_null())
                    {
                        error = "chain[" + std::to_string(index) + "] lane mask is not an object";
                        return false;
                    }
                }
            }
            if (!node.contains("tool") || !node["tool"].is_string() || node["tool"].get<std::string>().empty())
            {
                error = "chain[" + std::to_string(index) + "] has no tool";
                return false;
            }
            if (node.contains("id") && !node["id"].is_string())
            {
                error = "chain[" + std::to_string(index) + "] id is not a string";
                return false;
            }
            if (node.contains("enabled") && !node["enabled"].is_boolean())
            {
                error = "chain[" + std::to_string(index) + "] enabled is not a boolean";
                return false;
            }
            if (node.contains("params") && !node["params"].is_object())
            {
                error = "chain[" + std::to_string(index) + "] params is not an object";
                return false;
            }
            if (node.contains("blend") && !node["blend"].is_string())
            {
                error = "chain[" + std::to_string(index) + "] blend is not a string";
                return false;
            }
            if (node.contains("strength") && !node["strength"].is_number())
            {
                error = "chain[" + std::to_string(index) + "] strength is not a number";
                return false;
            }
            return true;
        }

        // Every node, lane and lane step in the tree: shape and id uniqueness.
        bool validateChainShape(const nlohmann::json &chain, const std::string &where, std::set<std::string> &ids,
                                std::string &error)
        {
            if (!chain.is_array())
            {
                error = where + " is not an array";
                return false;
            }
            for (size_t i = 0; i < chain.size(); ++i)
            {
                const nlohmann::json &node = chain[i];
                if (!validateNodeShape(node, i, error))
                    return false;
                if (node.contains("id"))
                {
                    std::string id = node["id"].get<std::string>();
                    if (!id.empty() && !ids.insert(id).second)
                    {
                        error = "duplicate node id '" + id + "'";
                        return false;
                    }
                }
                if (node.contains("lanes"))
                    for (const auto &lane : node["lanes"])
                    {
                        if (lane.contains("id") && lane["id"].is_string())
                        {
                            std::string id = lane["id"].get<std::string>();
                            if (!id.empty() && !ids.insert(id).second)
                            {
                                error = "duplicate lane id '" + id + "'";
                                return false;
                            }
                        }
                        if (lane.contains("steps") && !validateChainShape(lane["steps"], where + " lane steps", ids, error))
                            return false;
                    }
            }
            return true;
        }
    }

    bool AdjustmentDocument::parse(const std::string &text, AdjustmentDocument &out, std::string &error)
    {
        nlohmann::json j;
        try
        {
            j = nlohmann::json::parse(text);
        }
        catch (const std::exception &e)
        {
            error = std::string("document is not valid JSON: ") + e.what();
            return false;
        }
        if (!j.is_object())
        {
            error = "document is not a JSON object";
            return false;
        }
        if (j.value("format", std::string()) != kFormat)
        {
            error = "document format is not '" + std::string(kFormat) + "'";
            return false;
        }
        int version = j.value("version", 0);
        if (version < 1)
        {
            error = "document has no version";
            return false;
        }
        if (version > kVersion)
        {
            error = "document version " + std::to_string(version) + " is newer than supported " +
                    std::to_string(kVersion);
            return false;
        }
        AdjustmentDocument doc;
        doc.version = kVersion; // whatever came in, this build writes format 2
        if (j.contains("asset") && j["asset"].is_object())
        {
            const auto &a = j["asset"];
            doc.asset_name = a.value("name", std::string());
            doc.asset_relative_path = a.value("relative_path", std::string());
            doc.asset_size_bytes = a.value("size_bytes", int64_t(0));
        }
        doc.updated_at = j.value("updated_at", std::string());

        if (version == 1)
        {
            // Format 1: a map keyed by tool id, rendered in the registry's
            // fixed order (rotate, then crop). Unknown sections follow.
            if (j.contains("tools") && !j["tools"].is_object())
            {
                error = "document 'tools' is not an object";
                return false;
            }
            nlohmann::json tools = j.value("tools", nlohmann::json::object());
            auto take = [&](const std::string &id)
            {
                if (!tools.contains(id))
                    return;
                nlohmann::json node = nlohmann::json::object();
                node["tool"] = id;
                node["params"] = tools[id];
                doc.chain.push_back(node);
                tools.erase(id);
            };
            take("rotate");
            take("crop");
            for (auto it = tools.begin(); it != tools.end(); ++it)
            {
                nlohmann::json node = nlohmann::json::object();
                node["tool"] = it.key();
                node["params"] = it.value();
                doc.chain.push_back(node);
            }
        }
        else
        {
            if (j.contains("chain"))
            {
                if (!j["chain"].is_array())
                {
                    error = "document 'chain' is not an array";
                    return false;
                }
                doc.chain = j["chain"];
            }
            if (j.contains("history") && j["history"].is_object())
                doc.history = j["history"];
        }

        std::set<std::string> ids;
        if (!validateChainShape(doc.chain, "chain", ids, error))
            return false;
        doc.ensureNodeIds();
        out = std::move(doc);
        return true;
    }

    std::string AdjustmentDocument::serialize() const
    {
        nlohmann::json j;
        j["format"] = kFormat;
        j["version"] = kVersion;
        nlohmann::json a;
        a["name"] = asset_name;
        a["relative_path"] = asset_relative_path;
        a["size_bytes"] = asset_size_bytes;
        j["asset"] = a;
        j["updated_at"] = updated_at;
        j["chain"] = rounded(chain);
        if (hasHistory())
            j["history"] = rounded(history);
        return j.dump(2);
    }

    std::string AdjustmentDocument::serializeChain() const
    {
        return rounded(chain).dump();
    }

    std::string AdjustmentDocument::canonicalOf(const nlohmann::json &chain)
    {
        return canonicalChainArray(chain).dump();
    }

    std::string AdjustmentDocument::canonicalChain() const
    {
        return canonicalOf(chain);
    }

    uint32_t AdjustmentDocument::fnv1a32(const std::string &text)
    {
        uint32_t h = 2166136261u;
        for (unsigned char c : text)
        {
            h ^= c;
            h *= 16777619u;
        }
        return h;
    }

    int AdjustmentDocument::revision() const
    {
        if (isIdentity(ToolRegistry::instance()))
            return 0;
        uint32_t h = fnv1a32(canonicalChain());
        int rev = static_cast<int>(h & 0x7fffffffu);
        return rev == 0 ? 1 : rev;
    }

    namespace
    {
        bool compileChain(const nlohmann::json &chain, const ToolRegistry &registry, RenderPlan &out,
                          std::string &error, bool top_level);

        bool compileSplit(const nlohmann::json &node, const ToolRegistry &registry, RenderPlan::Node &n, std::string &error)
        {
            std::string tag = n.id.empty() ? std::string() : "node=" + n.id + " ";
            nlohmann::json lanes = node.value("lanes", nlohmann::json::array());
            for (const auto &lane : lanes)
            {
                if (!lane.is_object() || !nodeEnabled(lane))
                    continue;
                RenderPlan::Lane l;
                l.id = lane.value("id", std::string());
                l.name = lane.value("name", std::string());
                std::string merr;
                if (!LaneMask::parse(lane.value("mask", nlohmann::json()), l.mask, merr))
                {
                    error = tag + "lane " + l.id + ": " + merr;
                    return false;
                }
                nlohmann::json merge = lane.value("merge", nlohmann::json::object());
                if (merge.is_object())
                {
                    if (merge.contains("blend") && merge["blend"].is_string())
                        l.blend = merge["blend"].get<std::string>();
                    if (merge.contains("strength") && merge["strength"].is_number())
                        l.strength = merge["strength"].get<double>();
                }
                l.steps = std::make_shared<RenderPlan>();
                if (!compileChain(lane.value("steps", nlohmann::json::array()), registry, *l.steps, error, false))
                    return false;
                // A lane with nothing to do and a plain merge changes no
                // pixel; one with a non-normal merge still blends the fork
                // onto the trunk (a self-multiply darkens), so it stays.
                if (l.steps->isIdentity() && l.blend == "normal")
                    continue;
                if (l.strength <= 0.0)
                    continue;
                n.lanes.push_back(std::move(l));
            }
            return true;
        }

        bool compileChain(const nlohmann::json &chain, const ToolRegistry &registry, RenderPlan &out,
                          std::string &error, bool top_level)
        {
            RenderPlan plan;
            nlohmann::json canonical_prefix = nlohmann::json::array();
            for (size_t i = 0; chain.is_array() && i < chain.size(); ++i)
            {
                const nlohmann::json &node = chain[i];
                if (!node.is_object() || !nodeEnabled(node))
                    continue;
                std::string tool_id = node.value("tool", std::string());
                std::string id = node.value("id", std::string());
                auto tag = [&]() { return id.empty() ? std::string() : "node=" + id + " "; };
                canonical_prefix.push_back(canonicalNode(node));
                if (tool_id == AdjustmentDocument::kSplitTool)
                {
                    RenderPlan::Node n;
                    n.id = id;
                    if (!compileSplit(node, registry, n, error))
                        return false;
                    n.prefix_hash = std::to_string(AdjustmentDocument::fnv1a32(canonical_prefix.dump()));
                    n.doc_index = i;
                    if (!n.lanes.empty())
                        plan.add(std::move(n));
                    continue;
                }
                const AdjustmentTool *tool = registry.find(tool_id);
                if (!tool)
                    continue; // unknown tool: preserved in the document, a no-op in the render
                std::string tool_error;
                std::unique_ptr<ToolState> state = tool->parse(node.value("params", nlohmann::json::object()), tool_error);
                if (!state)
                {
                    error = tag() + tool_error;
                    return false;
                }
                if (tool->isIdentity(*state))
                    continue;
                RenderPlan::Node n;
                n.id = id;
                n.tool = tool;
                n.state = std::move(state);
                if (node.contains("blend"))
                    n.blend = node["blend"].get<std::string>();
                if (node.contains("strength"))
                    n.strength = node["strength"].get<double>();
                n.prefix_hash = std::to_string(AdjustmentDocument::fnv1a32(canonical_prefix.dump()));
                n.doc_index = i;
                plan.add(std::move(n));
            }
            (void)top_level;
            out = std::move(plan);
            return true;
        }
    }

    bool AdjustmentDocument::compile(const ToolRegistry &registry, RenderPlan &out, std::string &error) const
    {
        RenderPlan plan;
        if (!compileChain(chain, registry, plan, error, true))
            return false;
        plan.setRevision(revision());
        out = std::move(plan);
        return true;
    }

    bool AdjustmentDocument::isIdentity(const ToolRegistry &registry) const
    {
        if (!chain.is_array())
            return true;
        for (const auto &node : chain)
        {
            if (!node.is_object() || !nodeEnabled(node))
                continue;
            std::string tool_id = node.value("tool", std::string());
            if (tool_id == kSplitTool)
            {
                // A split renders something as soon as one enabled lane has
                // an enabled non-identity step, or merges with a blend other
                // than normal (the fork blended onto the trunk is a change).
                for (const auto &lane : node.value("lanes", nlohmann::json::array()))
                {
                    if (!lane.is_object() || !nodeEnabled(lane))
                        continue;
                    nlohmann::json merge = lane.value("merge", nlohmann::json::object());
                    double strength = merge.is_object() && merge.contains("strength") && merge["strength"].is_number()
                                          ? merge["strength"].get<double>() : 1.0;
                    if (strength <= 0.0)
                        continue;
                    std::string blend = merge.is_object() ? merge.value("blend", std::string("normal")) : "normal";
                    if (blend != "normal")
                        return false;
                    AdjustmentDocument sub;
                    sub.chain = lane.value("steps", nlohmann::json::array());
                    if (!sub.isIdentity(registry))
                        return false;
                }
                continue;
            }
            const AdjustmentTool *tool = registry.find(tool_id);
            if (!tool)
                return false; // an unknown tool is data we cannot call identity
            std::string error;
            std::unique_ptr<ToolState> state = tool->parse(node.value("params", nlohmann::json::object()), error);
            if (!state)
                return false; // unparseable is not identity either
            if (!tool->isIdentity(*state))
                return false;
        }
        return true;
    }

    bool AdjustmentDocument::hasHistory() const
    {
        return history.is_object() && history.contains("steps") && history["steps"].is_array() &&
               !history["steps"].empty();
    }

    std::string AdjustmentDocument::newNodeId()
    {
        static const char *alphabet = "abcdefghijklmnopqrstuvwxyz234567";
        static thread_local std::mt19937 rng{std::random_device{}()};
        std::uniform_int_distribution<int> pick(0, 31);
        std::string id;
        for (int i = 0; i < 8; ++i)
            id.push_back(alphabet[pick(rng)]);
        return id;
    }

    namespace
    {
        void collectIds(const nlohmann::json &chain, std::set<std::string> &ids)
        {
            if (!chain.is_array())
                return;
            for (const auto &node : chain)
            {
                if (!node.is_object())
                    continue;
                if (node.contains("id") && node["id"].is_string())
                    ids.insert(node["id"].get<std::string>());
                if (node.contains("lanes") && node["lanes"].is_array())
                    for (const auto &lane : node["lanes"])
                    {
                        if (lane.is_object() && lane.contains("id") && lane["id"].is_string())
                            ids.insert(lane["id"].get<std::string>());
                        if (lane.is_object() && lane.contains("steps"))
                            collectIds(lane["steps"], ids);
                    }
            }
        }

        void assignIds(nlohmann::json &chain, std::set<std::string> &ids)
        {
            if (!chain.is_array())
                return;
            auto fresh = [&ids]()
            {
                std::string id;
                do
                    id = AdjustmentDocument::newNodeId();
                while (!ids.insert(id).second);
                return id;
            };
            for (auto &node : chain)
            {
                if (!node.is_object())
                    continue;
                if (!node.contains("id") || !node["id"].is_string() || node["id"].get<std::string>().empty())
                    node["id"] = fresh();
                if (node.contains("lanes") && node["lanes"].is_array())
                    for (auto &lane : node["lanes"])
                    {
                        if (!lane.is_object())
                            continue;
                        if (!lane.contains("id") || !lane["id"].is_string() || lane["id"].get<std::string>().empty())
                            lane["id"] = fresh();
                        if (lane.contains("steps"))
                            assignIds(lane["steps"], ids);
                    }
            }
        }
    }

    void AdjustmentDocument::ensureNodeIds()
    {
        std::set<std::string> ids;
        collectIds(chain, ids);
        assignIds(chain, ids);
    }

    int AdjustmentDocument::historyIndex() const
    {
        if (!hasHistory())
            return -1;
        int idx = history.value("index", -1);
        int count = historyCount();
        if (idx < 0 || idx >= count)
            return count - 1;
        return idx;
    }

    int AdjustmentDocument::historyCount() const
    {
        return hasHistory() ? static_cast<int>(history["steps"].size()) : 0;
    }

    std::vector<std::pair<std::string, std::string>> AdjustmentDocument::historySteps() const
    {
        std::vector<std::pair<std::string, std::string>> out;
        if (!hasHistory())
            return out;
        for (const auto &step : history["steps"])
            out.emplace_back(step.value("label", std::string()), step.value("at", std::string()));
        return out;
    }

    void AdjustmentDocument::appendHistoryStep(const std::string &label, int max_entries)
    {
        if (!history.is_object())
            history = nlohmann::json::object();
        if (!history.contains("steps") || !history["steps"].is_array())
            history["steps"] = nlohmann::json::array();
        auto &steps = history["steps"];
        if (steps.empty())
        {
            nlohmann::json original;
            original["at"] = updated_at.empty() ? nowIso8601() : updated_at;
            original["label"] = "Original";
            original["chain"] = nlohmann::json::array();
            steps.push_back(original);
            history["index"] = 0;
        }
        int index = historyIndex();
        // The same chain committed twice (a client flush racing a click) is
        // one step, not two: the user never did two things.
        if (index >= 0 && index < static_cast<int>(steps.size()) &&
            canonicalOf(steps[static_cast<size_t>(index)].value("chain", nlohmann::json::array())) == canonicalOf(chain) &&
            rounded(steps[static_cast<size_t>(index)].value("chain", nlohmann::json::array())).dump() == rounded(chain).dump())
            return;
        // Drop the redo tail.
        while (static_cast<int>(steps.size()) > index + 1)
            steps.erase(steps.size() - 1);
        nlohmann::json step;
        step["at"] = nowIso8601();
        step["label"] = label.empty() ? "Edit" : label;
        step["chain"] = chain;
        steps.push_back(step);
        if (max_entries < 2)
            max_entries = 2;
        while (static_cast<int>(steps.size()) > max_entries)
            steps.erase(0);
        history["index"] = static_cast<int>(steps.size()) - 1;
    }

    bool AdjustmentDocument::jumpHistory(int index)
    {
        if (!hasHistory() || index < 0 || index >= historyCount())
            return false;
        const auto &step = history["steps"][static_cast<size_t>(index)];
        chain = step.value("chain", nlohmann::json::array());
        history["index"] = index;
        ensureNodeIds();
        return true;
    }

    std::string AdjustmentDocument::nowIso8601()
    {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
        gmtime_r(&t, &tm);
        std::ostringstream os;
        os << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
        return os.str();
    }
}
