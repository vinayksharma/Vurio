#pragma once

// ADJ-1 (docs §4): the per-file adjustment document — the sidecar's body and
// the `image_adjustments.document` column. Known tool sections are parsed by
// their tools; unknown ones are preserved verbatim so a newer build's edits
// survive an older build's save.

#include "adjustments/render_chain.hpp"
#include "adjustments/tool_registry.hpp"
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>

namespace MediaDedup::Adjustments
{
    struct AdjustmentDocument
    {
        static constexpr const char *kFormat = "vurio-adjustments";
        static constexpr int kVersion = 1;

        int version = kVersion;
        std::string asset_name;           // "DSC04624.JPG"
        std::string asset_relative_path;  // under the registered location
        int64_t asset_size_bytes = 0;
        std::string updated_at;           // ISO-8601 UTC, informational
        nlohmann::json tools = nlohmann::json::object(); // id -> section

        // Parse the sidecar/column text. Refuses a newer format version.
        static bool parse(const std::string &text, AdjustmentDocument &out, std::string &error);

        // Full document text as written to the sidecar (pretty, sorted keys).
        std::string serialize() const;

        // The canonical form of the tools map: sorted keys, numbers rounded
        // to 6 decimals, no whitespace. Identical documents on any machine
        // hash the same (docs §3.4).
        std::string canonicalTools() const;

        // Derived render revision: FNV-1a of canonicalTools() folded to a
        // positive int31; 0 iff the document is identity.
        int revision() const;

        // Compile the known, non-identity tool sections into a chain in
        // registry order. Fails on a malformed or out-of-range section.
        bool compile(const ToolRegistry &registry, RenderChain &out, std::string &error) const;

        // Identity: no unknown sections and every known section is identity.
        bool isIdentity(const ToolRegistry &registry) const;

        // Replace one tool's section from a state (re-serialized by the tool).
        void setTool(const AdjustmentTool &tool, const ToolState &state);
        void removeTool(const std::string &tool_id);

        static uint32_t fnv1a32(const std::string &text);
        static std::string nowIso8601();
    };
}
