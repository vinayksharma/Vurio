#include "adjustments/adjustment_document.hpp"
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
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
        doc.version = version;
        if (j.contains("asset") && j["asset"].is_object())
        {
            const auto &a = j["asset"];
            doc.asset_name = a.value("name", std::string());
            doc.asset_relative_path = a.value("relative_path", std::string());
            doc.asset_size_bytes = a.value("size_bytes", int64_t(0));
        }
        doc.updated_at = j.value("updated_at", std::string());
        if (j.contains("tools"))
        {
            if (!j["tools"].is_object())
            {
                error = "document 'tools' is not an object";
                return false;
            }
            doc.tools = j["tools"];
        }
        out = std::move(doc);
        return true;
    }

    std::string AdjustmentDocument::serialize() const
    {
        nlohmann::json j;
        j["format"] = kFormat;
        j["version"] = version;
        nlohmann::json a;
        a["name"] = asset_name;
        a["relative_path"] = asset_relative_path;
        a["size_bytes"] = asset_size_bytes;
        j["asset"] = a;
        j["updated_at"] = updated_at;
        j["tools"] = rounded(tools);
        return j.dump(2);
    }

    std::string AdjustmentDocument::canonicalTools() const
    {
        return rounded(tools).dump();
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
        uint32_t h = fnv1a32(canonicalTools());
        int rev = static_cast<int>(h & 0x7fffffffu);
        return rev == 0 ? 1 : rev;
    }

    bool AdjustmentDocument::compile(const ToolRegistry &registry, RenderChain &out, std::string &error) const
    {
        RenderChain chain;
        for (const AdjustmentTool *tool : registry.tools())
        {
            auto it = tools.find(tool->id());
            if (it == tools.end())
                continue;
            std::unique_ptr<ToolState> state = tool->parse(it.value(), error);
            if (!state)
                return false;
            if (tool->isIdentity(*state))
                continue;
            chain.add(tool, std::move(state));
        }
        chain.setRevision(revision());
        out = std::move(chain);
        return true;
    }

    bool AdjustmentDocument::isIdentity(const ToolRegistry &registry) const
    {
        for (auto it = tools.begin(); it != tools.end(); ++it)
        {
            const AdjustmentTool *tool = registry.find(it.key());
            if (!tool)
                return false; // an unknown tool is data we cannot call identity
            std::string error;
            std::unique_ptr<ToolState> state = tool->parse(it.value(), error);
            if (!state)
                return false; // unparseable is not identity either
            if (!tool->isIdentity(*state))
                return false;
        }
        return true;
    }

    void AdjustmentDocument::setTool(const AdjustmentTool &tool, const ToolState &state)
    {
        tools[tool.id()] = tool.serialize(state);
    }

    void AdjustmentDocument::removeTool(const std::string &tool_id)
    {
        tools.erase(tool_id);
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
