#include "adjustments/tool_registry.hpp"
#include "adjustments/tools/crop_tool.hpp"
#include "adjustments/tools/layer_tools.hpp"
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
        // Catalogue order: geometry, then the layer tools, then raster.
        owned_.push_back(std::make_unique<RotateTool>());
        owned_.push_back(std::make_unique<CropTool>());
        owned_.push_back(std::make_unique<ExposureTool>());
        owned_.push_back(std::make_unique<BrightContrastTool>());
        owned_.push_back(std::make_unique<WhiteBalanceTool>());
        owned_.push_back(std::make_unique<SaturationTool>());
        owned_.push_back(std::make_unique<HueTool>());
        owned_.push_back(std::make_unique<MonochromeTool>());
        owned_.push_back(std::make_unique<ColorFilterTool>());
        owned_.push_back(std::make_unique<GammaTool>());
        owned_.push_back(std::make_unique<VignetteTool>());
        owned_.push_back(std::make_unique<SharpenTool>());
        owned_.push_back(std::make_unique<DenoiseTool>());
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
