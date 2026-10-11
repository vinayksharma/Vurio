#pragma once

// CHAIN-1 (docs/todo_tool_chain.md §3.3, §5.2): a document compiled into the
// user's node order. This is the only thing the render paths consume: the
// size transform for display geometry, the validation rules that need the
// image, the revision that keys every cache, and `render`, which runs the
// nodes through the engine. Blend and strength are applied here, per node,
// so a tool only ever produces its derived frame.
//
// A node is a tool node or a split node (CHAIN-4): lanes forked from the
// split's input, each with a mask, its own sub-plan and a merge, mixed back
// onto the trunk in lane order (docs §3.3).

#include "adjustments/adjustment_tool.hpp"
#include "adjustments/masks.hpp"
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
        struct Lane;
        struct Node
        {
            std::string id;                        // document node id (empty for synthetic plans)
            const AdjustmentTool *tool = nullptr;  // null for a split node
            std::unique_ptr<ToolState> state;
            std::string blend = "normal";          // layer/raster only
            double strength = 1.0;                 // layer/raster only
            std::vector<Lane> lanes;               // split node only
            std::string prefix_hash;               // canonical hash of nodes[0..this], set by the compiler
            size_t doc_index = 0;                  // position in the document's chain, set by the compiler
            bool isSplit() const { return tool == nullptr; }
        };
        struct Lane
        {
            std::string id;
            std::string name;
            LaneMask mask;
            std::shared_ptr<RenderPlan> steps;     // the lane's sub-plan
            std::string blend = "normal";          // merge
            double strength = 1.0;                 // merge
        };

        // View modes the preview asks for (docs §5.4): render a lane alone
        // over mid-grey, or tint a lane's coverage red on the normal render.
        struct RenderOptions
        {
            std::string solo_lane_id;
            std::string overlay_lane_id;
            // CHAIN-5: detected masks are looked up by file; a preview
            // never computes one inline (the lane renders as a no-op and
            // its id lands in `pending_lanes`), a thumbnail or full render
            // does.
            std::string file_path;
            bool inline_masks;
            std::shared_ptr<std::vector<std::string>> pending_lanes;
            RenderOptions() : inline_masks(true) {}
            bool any() const { return !solo_lane_id.empty() || !overlay_lane_id.empty(); }
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

        // Append a non-identity node (a tool node or a split). Order of
        // calls is the user's order.
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
        // following crop that lies inside the rotated content, no geometry
        // inside a lane, lanes nest at most `kMaxLaneDepth` deep. A base of
        // unknown size skips the geometry check (it cannot be verified).
        bool validate(Size base, std::string &error) const { return validate(base, error, 0); }
        static constexpr int kMaxLaneDepth = 3;

        // Run nodes [first, size()) on `frame` in place. `first` lets a
        // caller resume from a cached intermediate frame (§5.4). After each
        // node `observer`, when set, sees the index and the frame.
        using Observer = std::function<void(size_t index, const Frame &after)>;
        void render(Frame &frame, size_t first = 0, const Observer &observer = nullptr,
                    const RenderOptions &options = RenderOptions()) const;

        // Does a lane with this id exist anywhere in the plan?
        bool hasLane(const std::string &lane_id) const;

        // The blend names the engine implements (CHAIN-1: "normal" only;
        // CHAIN-2 adds the sixteen modes of docs §5.3).
        static const std::vector<std::string> &blendModes();
        static bool isBlendMode(const std::string &name);

    private:
        bool validate(Size base, std::string &error, int depth) const;
        static void applyNode(const Node &node, Frame &frame, const RenderOptions &options);
        static void applySplit(const Node &node, Frame &frame, const RenderOptions &options);
        std::vector<Node> nodes_;
        int revision_ = 0;
    };
}
