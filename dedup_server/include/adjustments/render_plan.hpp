#pragma once

// CHAIN-1 (docs/todo_tool_chain.md §3.3, §5.2): a document compiled into the
// user's node order. This is the only thing the render paths consume: the
// size transform for display geometry, the validation rules that need the
// image, the revision that keys every cache, and `render`, which runs the
// nodes through the engine. Blend and strength are applied here, per node,
// so a tool only ever produces its derived frame.
//
// Format 2 keeps the node tree in mind (splits with lanes land in CHAIN-4);
// in CHAIN-1 a plan is a list of tool nodes.

#include "adjustments/adjustment_tool.hpp"
#include "utils/display_geometry.hpp"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace MediaDedup::Adjustments
{
    class RenderPlan
    {
    public:
        struct Node
        {
            std::string id;                        // document node id (empty for synthetic plans)
            const AdjustmentTool *tool = nullptr;
            std::unique_ptr<ToolState> state;
            std::string blend = "normal";          // layer/raster only
            double strength = 1.0;                 // layer/raster only
            std::string prefix_hash;               // canonical hash of nodes[0..this], set by the compiler
            size_t doc_index = 0;                  // position in the document's chain, set by the compiler
        };

        RenderPlan() = default;
        RenderPlan(const RenderPlan &other);
        RenderPlan &operator=(const RenderPlan &other);
        RenderPlan(RenderPlan &&) = default;
        RenderPlan &operator=(RenderPlan &&) = default;

        // The plan every un-edited file has.
        static const RenderPlan &identity();

        bool isIdentity() const { return nodes_.empty(); }
        int revision() const { return revision_; }
        void setRevision(int revision) { revision_ = revision; }

        // Append a non-identity node. Order of calls is the user's order.
        void add(Node node);
        const std::vector<Node> &nodes() const { return nodes_; }
        size_t size() const { return nodes_.size(); }

        // The plan truncated before `count` nodes (crop editing: the canvas
        // a node receives). count >= size() returns a copy.
        RenderPlan prefix(size_t count) const;

        // First node of a tool, or null.
        const ToolState *firstStateOf(const std::string &tool_id) const;

        // Display geometry: the size of the rendered result from the
        // auto-oriented base size. Unknown base (<= 0) stays unknown.
        Size displaySize(Size base) const;
        DisplayDims displayDims(int base_width, int base_height) const;
        // Canvas entering node `index` (index == size() → the output).
        Size sizeBefore(size_t index, Size base) const;

        // Placement and cross-node rules that need the image (§3.2, §3.3):
        // blend/strength refused on geometry, a straighten angle needs a
        // following crop that lies inside the rotated content. A base of
        // unknown size skips the geometry check (it cannot be verified).
        bool validate(Size base, std::string &error) const;

        // Run nodes [first, size()) on `frame` in place. `first` lets a
        // caller resume from a cached intermediate frame (§5.4). After each
        // node `observer`, when set, sees the index and the frame.
        using Observer = std::function<void(size_t index, const Frame &after)>;
        void render(Frame &frame, size_t first = 0, const Observer &observer = nullptr) const;

        // The blend names the engine implements (CHAIN-1: "normal" only;
        // CHAIN-2 adds the sixteen modes of docs §5.3).
        static const std::vector<std::string> &blendModes();
        static bool isBlendMode(const std::string &name);

    private:
        static void applyNode(const Node &node, Frame &frame);
        std::vector<Node> nodes_;
        int revision_ = 0;
    };
}
