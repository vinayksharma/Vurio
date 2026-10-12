#pragma once

// CHAIN-1 (docs/todo_tool_chain.md §4): the per-file adjustment document —
// the sidecar's body and the `image_adjustments.document` column. Format 2:
// an ordered `chain` of nodes (id, tool, version, enabled, params, and for
// layer/raster nodes blend + strength), plus the photo's persisted undo
// history. Known tools are parsed by their tools; unknown nodes and unknown
// keys are preserved verbatim so a newer build's edits survive an older
// build's save. Format-1 documents (a `tools` map) load as the fixed
// [rotate, crop] order they rendered in and are rewritten as format 2 on the
// next commit.

#include "adjustments/render_plan.hpp"
#include "adjustments/tool_registry.hpp"
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

namespace MediaDedup::Adjustments
{
    struct AdjustmentDocument
    {
        static constexpr const char *kFormat = "vurio-adjustments";
        static constexpr int kVersion = 2;
        static constexpr const char *kSplitTool = "split";

        int version = kVersion;
        std::string asset_name;           // "DSC04624.JPG"
        std::string asset_relative_path;  // under the registered location
        int64_t asset_size_bytes = 0;
        std::string updated_at;           // ISO-8601 UTC, informational
        nlohmann::json chain = nlohmann::json::array();   // nodes, the user's order
        // {"index": n, "steps": [{at, label, chain}], "snapshots": [{id, name, at, chain}]} or null
        nlohmann::json history = nlohmann::json();

        // Parse the sidecar/column text (format 1 or 2). Refuses a newer
        // format version, a malformed chain, and duplicate node ids. Nodes
        // without an id get one.
        static bool parse(const std::string &text, AdjustmentDocument &out, std::string &error);

        // Full document text as written to the sidecar (pretty, sorted keys).
        std::string serialize() const;
        // The chain alone, as the client sends and receives it.
        std::string serializeChain() const;

        // The canonical form of the EFFECTIVE chain (disabled nodes dropped;
        // ids, names and defaults omitted; sorted keys; numbers rounded to 6
        // decimals; no whitespace). Identical documents on any machine hash
        // the same (docs §3.5).
        std::string canonicalChain() const;
        static std::string canonicalOf(const nlohmann::json &chain);

        // Derived render revision: FNV-1a of canonicalChain() folded to a
        // positive int31; 0 iff the effective chain renders nothing.
        int revision() const;

        // Compile the enabled, known, non-identity nodes into a plan in
        // document order. Fails on a malformed or out-of-range node, a
        // blend/strength on a geometry node, or a split (CHAIN-4).
        bool compile(const ToolRegistry &registry, RenderPlan &out, std::string &error) const;

        // Identity: every enabled node is a known tool at its identity.
        bool isIdentity(const ToolRegistry &registry) const;
        bool hasNodes() const { return chain.is_array() && !chain.empty(); }
        bool hasHistory() const;
        bool hasSnapshots() const;
        // Nothing to keep: no nodes, no history, no snapshots → the row and
        // sidecar go.
        bool isEmpty() const { return !hasNodes() && !hasHistory() && !hasSnapshots(); }

        // Node ids: 8 base32 characters, unique within the document.
        static std::string newNodeId();
        void ensureNodeIds();

        // ---- history (docs §4, §9.2) -------------------------------------
        int historyIndex() const;
        int historyCount() const;
        std::vector<std::pair<std::string, std::string>> historySteps() const; // (label, at)
        // Snapshot the current chain as a new step after the current index
        // (the redo tail is dropped), capped at `max_entries` (oldest go).
        // A document without history first records an "Original" step with
        // an empty chain so the pristine state can be returned to.
        void appendHistoryStep(const std::string &label, int max_entries);
        // Make step `index` the current chain. False when out of range.
        bool jumpHistory(int index);

        // ---- snapshots (CHAIN-7e) ----------------------------------------
        // Named copies of the chain beside the undo stack: they survive the
        // step cap and the redo truncation, and restoring one is an ordinary
        // edit (the caller appends the step). Stored as
        // history.snapshots[{id, name, at, chain}].
        struct Snapshot
        {
            std::string id;
            std::string name;
            std::string at;
            int node_count = 0;
        };
        std::vector<Snapshot> snapshots() const;
        // Copy the current chain under `name`; the oldest snapshots go past
        // `max_snapshots`. Returns the new id.
        std::string saveSnapshot(const std::string &name, int max_snapshots);
        // Make the snapshot's chain current (fresh node ids). False when unknown.
        bool restoreSnapshot(const std::string &id, std::string *name = nullptr);
        bool renameSnapshot(const std::string &id, const std::string &name);
        bool deleteSnapshot(const std::string &id);

        static uint32_t fnv1a32(const std::string &text);
        static std::string nowIso8601();
    };
}
