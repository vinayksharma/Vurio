#pragma once

// ADJ-1 (docs §5.3): a document compiled into the fixed application order.
// This is the only thing the render pipelines consume: ImageMagick
// operators for the pixels, a size transform for display geometry, and the
// revision that keys every cache.

#include "adjustments/adjustment_tool.hpp"
#include "utils/display_geometry.hpp"
#include <memory>
#include <string>
#include <vector>

namespace MediaDedup::Adjustments
{
    class RenderChain
    {
    public:
        struct Entry
        {
            const AdjustmentTool *tool = nullptr;
            std::unique_ptr<ToolState> state;
        };

        RenderChain() = default;
        RenderChain(const RenderChain &other);
        RenderChain &operator=(const RenderChain &other);
        RenderChain(RenderChain &&) = default;
        RenderChain &operator=(RenderChain &&) = default;

        // The chain every un-edited file has.
        static const RenderChain &identity();

        bool isIdentity() const { return entries_.empty(); }
        int revision() const { return revision_; }
        void setRevision(int revision) { revision_ = revision; }

        // Append a non-identity tool state. Order of calls must follow the
        // registry order; AdjustmentDocument::compile guarantees it.
        void add(const AdjustmentTool *tool, std::unique_ptr<ToolState> state);

        const std::vector<Entry> &entries() const { return entries_; }
        const ToolState *stateOf(const std::string &tool_id) const;

        // ImageMagick operators for the whole chain, in order.
        void appendMagickOps(std::vector<std::string> &args) const;

        // Display geometry: the size of the rendered result from the
        // auto-oriented base size. Unknown base (<= 0) stays unknown.
        Size displaySize(Size base) const;
        DisplayDims displayDims(int base_width, int base_height) const;

        // Cross-tool rules that need the image: a straighten angle requires
        // a crop, and the crop must lie inside the rotated content. A base
        // of unknown size skips the geometry check (it cannot be verified).
        bool validate(Size base, std::string &error) const;

    private:
        std::vector<Entry> entries_;
        int revision_ = 0;
    };
}
