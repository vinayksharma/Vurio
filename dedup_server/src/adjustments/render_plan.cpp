#include "adjustments/render_plan.hpp"
#include "adjustments/tools/crop_tool.hpp"
#include "adjustments/tools/rotate_tool.hpp"
#include <cmath>
#include <opencv2/imgproc.hpp>

namespace MediaDedup::Adjustments
{
    namespace
    {
        RenderPlan::Node cloneNode(const RenderPlan::Node &n)
        {
            RenderPlan::Node c;
            c.id = n.id;
            c.tool = n.tool;
            c.state = n.state ? n.state->clone() : nullptr;
            c.blend = n.blend;
            c.strength = n.strength;
            c.prefix_hash = n.prefix_hash;
            c.doc_index = n.doc_index;
            return c;
        }
    }

    RenderPlan::RenderPlan(const RenderPlan &other) : revision_(other.revision_)
    {
        for (const Node &n : other.nodes_)
            nodes_.push_back(cloneNode(n));
    }

    RenderPlan &RenderPlan::operator=(const RenderPlan &other)
    {
        if (this != &other)
        {
            nodes_.clear();
            for (const Node &n : other.nodes_)
                nodes_.push_back(cloneNode(n));
            revision_ = other.revision_;
        }
        return *this;
    }

    const RenderPlan &RenderPlan::identity()
    {
        static const RenderPlan empty;
        return empty;
    }

    void RenderPlan::add(Node node)
    {
        if (!node.tool || !node.state)
            return;
        nodes_.push_back(std::move(node));
    }

    RenderPlan RenderPlan::prefix(size_t count) const
    {
        RenderPlan out;
        for (size_t i = 0; i < nodes_.size() && i < count; ++i)
            out.nodes_.push_back(cloneNode(nodes_[i]));
        out.revision_ = revision_;
        return out;
    }

    const ToolState *RenderPlan::firstStateOf(const std::string &tool_id) const
    {
        for (const Node &n : nodes_)
            if (n.tool->id() == tool_id)
                return n.state.get();
        return nullptr;
    }

    Size RenderPlan::sizeBefore(size_t index, Size base) const
    {
        if (base.width <= 0 || base.height <= 0)
            return base;
        Size s = base;
        for (size_t i = 0; i < nodes_.size() && i < index; ++i)
            s = nodes_[i].tool->transformSize(*nodes_[i].state, s);
        return s;
    }

    Size RenderPlan::displaySize(Size base) const
    {
        return sizeBefore(nodes_.size(), base);
    }

    DisplayDims RenderPlan::displayDims(int base_width, int base_height) const
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

    bool RenderPlan::validate(Size base, std::string &error) const
    {
        auto nodeTag = [](const Node &n) { return n.id.empty() ? std::string() : "node=" + n.id + " "; };
        Size input = base;
        for (size_t i = 0; i < nodes_.size(); ++i)
        {
            const Node &n = nodes_[i];
            if (n.tool->category() == ToolCategory::Geometry &&
                (n.blend != "normal" || std::fabs(n.strength - 1.0) > 1e-9))
            {
                error = nodeTag(n) + n.tool->id() + ": a geometry node takes no blend or strength (docs §3.2)";
                return false;
            }
            if (!isBlendMode(n.blend))
            {
                error = nodeTag(n) + n.tool->id() + ": unknown blend mode '" + n.blend + "'";
                return false;
            }
            if (n.strength < 0.0 || n.strength > 1.0)
            {
                error = nodeTag(n) + n.tool->id() + ": strength must be within [0, 1]";
                return false;
            }
            std::string tool_error;
            if (!n.tool->validate(*n.state, input, tool_error))
            {
                error = nodeTag(n) + tool_error;
                return false;
            }

            // A straighten angle exposes empty corners; the next geometry
            // node must be a crop that lies inside the rotated content.
            if (n.tool->id() == RotateTool::kId)
            {
                const auto &rotate = static_cast<const RotateState &>(*n.state);
                if (std::fabs(rotate.angle) >= 1e-9)
                {
                    const Node *next_geometry = nullptr;
                    for (size_t j = i + 1; j < nodes_.size(); ++j)
                        if (nodes_[j].tool->category() == ToolCategory::Geometry)
                        {
                            next_geometry = &nodes_[j];
                            break;
                        }
                    if (!next_geometry || next_geometry->tool->id() != CropTool::kId)
                    {
                        error = nodeTag(n) + "rotate: a straighten angle requires a following crop (docs §3.3)";
                        return false;
                    }
                    const auto &crop = static_cast<const CropState &>(*next_geometry->state);
                    if (input.width > 0 && input.height > 0 &&
                        !cropInsideContent(input, rotate.quarter_turns, rotate.angle, crop.rect))
                    {
                        error = nodeTag(*next_geometry) + "crop: rectangle leaves the rotated image (empty corners)";
                        return false;
                    }
                }
            }
            input = n.tool->transformSize(*n.state, input);
        }
        return true;
    }

    const std::vector<std::string> &RenderPlan::blendModes()
    {
        // CHAIN-2 adds the fifteen others (docs §5.3) together with their
        // engine implementations; the list and the engine move together.
        static const std::vector<std::string> modes = {"normal"};
        return modes;
    }

    bool RenderPlan::isBlendMode(const std::string &name)
    {
        for (const auto &m : blendModes())
            if (m == name)
                return true;
        return false;
    }

    void RenderPlan::applyNode(const Node &node, Frame &frame)
    {
        RenderContext ctx;
        ctx.pixel_scale = frame.pixelScale();
        const bool composed = node.tool->category() != ToolCategory::Geometry &&
                              (node.blend != "normal" || node.strength < 1.0 - 1e-9);
        if (!composed)
        {
            node.tool->apply(*node.state, frame, ctx);
            return;
        }
        // Derived image, then the engine's composition: blend onto the
        // input (CHAIN-1 knows "normal" = the derived image itself), then
        // keep `strength` of the result: in + (blended − in) × s.
        Frame derived;
        derived.pixels = frame.pixels.clone();
        node.tool->apply(*node.state, derived, ctx);
        if (node.strength >= 1.0 - 1e-9)
        {
            frame.pixels = derived.pixels;
            return;
        }
        cv::addWeighted(frame.pixels, 1.0 - node.strength, derived.pixels, node.strength, 0.0, frame.pixels);
    }

    void RenderPlan::render(Frame &frame, size_t first, const Observer &observer) const
    {
        for (size_t i = first; i < nodes_.size(); ++i)
        {
            if (frame.empty())
                return;
            applyNode(nodes_[i], frame);
            if (observer)
                observer(i, frame);
        }
    }
}
