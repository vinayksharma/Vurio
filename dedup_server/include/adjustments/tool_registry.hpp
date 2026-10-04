#pragma once

// ADJ-1 (docs §3.2): the fixed application order of the tools. The only
// place in the server that names a concrete tool. Rotate runs before Crop:
// a crop is expressed on the rotated canvas.

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

        // Tools in application order.
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
