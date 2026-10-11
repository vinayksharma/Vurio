#include "adjustments/render_plan.hpp"
#include "adjustments/blend.hpp"
#include "adjustments/tools/crop_tool.hpp"
#include "adjustments/tools/rotate_tool.hpp"
#include <cmath>
#include <opencv2/imgproc.hpp>
#include <vector>

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
            for (const RenderPlan::Lane &l : n.lanes)
            {
                RenderPlan::Lane lc;
                lc.id = l.id;
                lc.name = l.name;
                lc.mask = l.mask;
                lc.steps = l.steps ? std::make_shared<RenderPlan>(*l.steps) : nullptr;
                lc.blend = l.blend;
                lc.strength = l.strength;
                c.lanes.push_back(std::move(lc));
            }
            c.prefix_hash = n.prefix_hash;
            c.doc_index = n.doc_index;
            return c;
        }

        // trunk = trunk + (blended − trunk) × strength × coverage, per pixel.
        void mixMasked(cv::Mat &trunk, const cv::Mat &blended, double strength, const cv::Mat &coverage)
        {
            cv::Mat weight = coverage * strength; // CV_32FC1
            cv::Mat weight3;
            cv::merge(std::vector<cv::Mat>{weight, weight, weight}, weight3);
            cv::Mat diff = blended - trunk;
            trunk += diff.mul(weight3);
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
        if (!node.isSplit() && !node.state)
            return;
        nodes_.push_back(std::move(node));
    }

    bool RenderPlan::hasLane(const std::string &lane_id) const
    {
        for (const Node &n : nodes_)
            for (const Lane &l : n.lanes)
                if (l.id == lane_id || (l.steps && l.steps->hasLane(lane_id)))
                    return true;
        return false;
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
            if (!nodes_[i].isSplit())
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

    bool RenderPlan::validate(Size base, std::string &error, int depth) const
    {
        auto nodeTag = [](const Node &n) { return n.id.empty() ? std::string() : "node=" + n.id + " "; };
        Size input = base;
        for (size_t i = 0; i < nodes_.size(); ++i)
        {
            const Node &n = nodes_[i];
            if (n.isSplit())
            {
                if (depth + 1 > kMaxLaneDepth)
                {
                    error = nodeTag(n) + "split: lanes nest at most " + std::to_string(kMaxLaneDepth) + " deep";
                    return false;
                }
                for (const Lane &l : n.lanes)
                {
                    if (!isBlendMode(l.blend))
                    {
                        error = nodeTag(n) + "lane " + l.id + ": unknown merge blend '" + l.blend + "'";
                        return false;
                    }
                    if (l.strength < 0.0 || l.strength > 1.0)
                    {
                        error = nodeTag(n) + "lane " + l.id + ": merge strength must be within [0, 1]";
                        return false;
                    }
                    if (l.steps)
                    {
                        for (const Node &step : l.steps->nodes())
                            if (!step.isSplit() && step.tool->category() == ToolCategory::Geometry)
                            {
                                error = "node=" + step.id + " " + step.tool->id() +
                                        ": a geometry tool cannot live in a lane, the lane must keep its canvas (docs §3.2)";
                                return false;
                            }
                        if (!l.steps->validate(input, error, depth + 1))
                            return false;
                    }
                }
                continue;
            }
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
            if (n.tool->category() == ToolCategory::Raster && n.blend != "normal")
            {
                error = nodeTag(n) + n.tool->id() + ": a raster node takes a strength but no blend mode (docs §3.2)";
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
                        if (!nodes_[j].isSplit() && nodes_[j].tool->category() == ToolCategory::Geometry)
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

    const std::vector<std::string> &RenderPlan::blendModes() { return Blend::modes(); }

    bool RenderPlan::isBlendMode(const std::string &name) { return Blend::isMode(name); }

    void RenderPlan::applySplit(const Node &node, Frame &frame, const RenderOptions &options)
    {
        // Lanes fork from the split's input and are mixed back in order: the
        // second lane sees the first's merge on the trunk, never in its own
        // input (docs §3.3).
        Frame fork;
        fork.pixels = frame.pixels.clone();
        fork.base_to_canvas = frame.base_to_canvas;
        for (const Lane &lane : node.lanes)
        {
            Frame laneOut;
            laneOut.pixels = fork.pixels.clone();
            laneOut.base_to_canvas = fork.base_to_canvas;
            if (lane.steps)
                lane.steps->render(laneOut, 0, nullptr, options);
            cv::Mat coverage;
            lane.mask.coverage(fork, coverage);

            if (lane.id == options.solo_lane_id && !options.solo_lane_id.empty())
            {
                // The lane alone over mid-grey, where its mask reaches.
                cv::Mat grey(frame.pixels.size(), CV_32FC3, cv::Scalar(0.5f, 0.5f, 0.5f));
                mixMasked(grey, laneOut.pixels, 1.0, coverage);
                frame.pixels = grey;
                return;
            }
            cv::Mat blended;
            if (lane.blend == "normal")
                blended = laneOut.pixels;
            else
                Blend::apply(lane.blend, frame.pixels, laneOut.pixels, blended);
            mixMasked(frame.pixels, blended, lane.strength, coverage);

            if (lane.id == options.overlay_lane_id && !options.overlay_lane_id.empty())
            {
                // Lightroom's convention: the coverage as a 50 % red tint.
                cv::Mat red(frame.pixels.size(), CV_32FC3, cv::Scalar(0.1f, 0.1f, 1.0f));
                mixMasked(frame.pixels, red, 0.5, coverage);
            }
        }
    }

    void RenderPlan::applyNode(const Node &node, Frame &frame, const RenderOptions &options)
    {
        if (node.isSplit())
        {
            applySplit(node, frame, options);
            return;
        }
        RenderContext ctx;
        ctx.pixel_scale = frame.pixelScale();
        const bool composed = node.tool->category() != ToolCategory::Geometry &&
                              (node.blend != "normal" || node.strength < 1.0 - 1e-9);
        if (!composed)
        {
            node.tool->apply(*node.state, frame, ctx);
            return;
        }
        // Derived image, then the engine's composition (docs §3.3): blend
        // onto the input, then keep `strength` of the result:
        // in + (blend(in, derived) − in) × s. A raster node has no blend
        // (always "normal"), only the mix.
        Frame derived;
        derived.pixels = frame.pixels.clone();
        node.tool->apply(*node.state, derived, ctx);
        cv::Mat blended;
        if (node.blend == "normal")
            blended = derived.pixels;
        else
            Blend::apply(node.blend, frame.pixels, derived.pixels, blended);
        if (node.strength >= 1.0 - 1e-9)
        {
            frame.pixels = blended;
            return;
        }
        cv::addWeighted(frame.pixels, 1.0 - node.strength, blended, node.strength, 0.0, frame.pixels);
    }

    void RenderPlan::render(Frame &frame, size_t first, const Observer &observer, const RenderOptions &options) const
    {
        for (size_t i = first; i < nodes_.size(); ++i)
        {
            if (frame.empty())
                return;
            applyNode(nodes_[i], frame, options);
            if (observer)
                observer(i, frame);
        }
    }
}
