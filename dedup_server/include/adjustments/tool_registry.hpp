#pragma once

// ADJ-1 / CHAIN-1 (docs/todo_tool_chain.md §5.6): the catalogue. The only
// place in the server that names a concrete tool. Since format 2 the order
// of application is the document's, not the registry's; `tools()` is the
// catalogue order (by category, then label) the UI shows.

#include "adjustments/adjustment_tool.hpp"
#include <memory>
#include <string>
#include <vector>

namespace MediaDedup::Adjustments
{
    class ToolRegistry
    {
    public:
        static const ToolRegistry &instance();

        // Catalogue order.
        const std::vector<const AdjustmentTool *> &tools() const { return order_; }

        // Tool by id, or null for an id this build does not know.
        const AdjustmentTool *find(const std::string &id) const;

        std::vector<ToolDescriptor> descriptors() const;

    private:
        ToolRegistry();
        std::vector<std::unique_ptr<AdjustmentTool>> owned_;
        std::vector<const AdjustmentTool *> order_;
    };
}
