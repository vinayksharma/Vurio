#include "adjustments/tool_registry.hpp"
#include "adjustments/tools/crop_tool.hpp"
#include "adjustments/tools/rotate_tool.hpp"

namespace MediaDedup::Adjustments
{
    const ToolRegistry &ToolRegistry::instance()
    {
        static const ToolRegistry registry;
        return registry;
    }

    ToolRegistry::ToolRegistry()
    {
        // Catalogue order: geometry first. CHAIN-2 appends the layer and
        // raster tools here.
        owned_.push_back(std::make_unique<RotateTool>());
        owned_.push_back(std::make_unique<CropTool>());
        for (const auto &t : owned_)
            order_.push_back(t.get());
    }

    const AdjustmentTool *ToolRegistry::find(const std::string &id) const
    {
        for (const AdjustmentTool *t : order_)
            if (t->id() == id)
                return t;
        return nullptr;
    }

    std::vector<ToolDescriptor> ToolRegistry::descriptors() const
    {
        std::vector<ToolDescriptor> out;
        for (const AdjustmentTool *t : order_)
            out.push_back(t->descriptor());
        return out;
    }
}
