#include "adjustments/render_chain.hpp"
#include "adjustments/tools/crop_tool.hpp"
#include "adjustments/tools/rotate_tool.hpp"
#include <cmath>

namespace MediaDedup::Adjustments
{
    RenderChain::RenderChain(const RenderChain &other) : revision_(other.revision_)
    {
        for (const Entry &e : other.entries_)
            entries_.push_back({e.tool, e.state->clone()});
    }

    RenderChain &RenderChain::operator=(const RenderChain &other)
    {
        if (this != &other)
        {
            entries_.clear();
            for (const Entry &e : other.entries_)
                entries_.push_back({e.tool, e.state->clone()});
            revision_ = other.revision_;
        }
        return *this;
    }

    const RenderChain &RenderChain::identity()
    {
        static const RenderChain empty;
        return empty;
    }

    void RenderChain::add(const AdjustmentTool *tool, std::unique_ptr<ToolState> state)
    {
        if (!tool || !state)
            return;
        entries_.push_back({tool, std::move(state)});
    }

    const ToolState *RenderChain::stateOf(const std::string &tool_id) const
    {
        for (const Entry &e : entries_)
            if (e.tool->id() == tool_id)
                return e.state.get();
        return nullptr;
    }

    void RenderChain::appendMagickOps(std::vector<std::string> &args) const
    {
        for (const Entry &e : entries_)
            e.tool->appendMagickOps(*e.state, args);
    }

    Size RenderChain::displaySize(Size base) const
    {
        if (base.width <= 0 || base.height <= 0)
            return base;
        Size s = base;
        for (const Entry &e : entries_)
            s = e.tool->transformSize(*e.state, s);
        return s;
    }

    DisplayDims RenderChain::displayDims(int base_width, int base_height) const
    {
        if (base_width <= 0 || base_height <= 0)
            return {};
        Size s = displaySize({static_cast<double>(base_width), static_cast<double>(base_height)});
        int w = static_cast<int>(std::lround(s.width));
        int h = static_cast<int>(std::lround(s.height));
        if (w <= 0 || h <= 0)
            return {};
        return {w, h};
    }

    bool RenderChain::validate(Size base, std::string &error) const
    {
        Size input = base;
        for (const Entry &e : entries_)
        {
            if (!e.tool->validate(*e.state, input, error))
                return false;
            input = e.tool->transformSize(*e.state, input);
        }

        const auto *rotate = static_cast<const RotateState *>(stateOf(RotateTool::kId));
        const auto *crop = static_cast<const CropState *>(stateOf(CropTool::kId));
        if (rotate && std::fabs(rotate->angle) >= 1e-9)
        {
            if (!crop)
            {
                error = "rotate: a straighten angle requires a crop section (docs §3.3)";
                return false;
            }
            if (base.width > 0 && base.height > 0 &&
                !cropInsideContent(base, rotate->quarter_turns, rotate->angle, crop->rect))
            {
                error = "crop: rectangle leaves the rotated image (empty corners)";
                return false;
            }
        }
        return true;
    }
}
