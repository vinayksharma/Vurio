#pragma once

// ADJ-1 / CHAIN-1 (docs/todo_tool_chain.md §5.2): the interface every
// adjustment tool implements. A tool is a self-contained unit: it owns its
// parameters and limits (descriptor), its JSON params (parse/serialize),
// its validation, and its application to a frame — ONE pixel implementation,
// in the server's engine, used for thumbnails, viewer renders, live preview
// frames and exports alike. Blend, strength, forks and merges are the
// engine's business (RenderPlan), never a tool's. The application never
// names a concrete tool outside the registry.

#include "adjustments/adjustment_geometry.hpp"
#include "adjustments/frame.hpp"
#include "adjustments/tool_descriptor.hpp"
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

namespace MediaDedup::Adjustments
{
    // A tool's parameter values for one node. Each tool defines its own
    // concrete state; the plan holds them behind this base.
    class ToolState
    {
    public:
        virtual ~ToolState() = default;
        virtual std::unique_ptr<ToolState> clone() const = 0;
    };

    class AdjustmentTool
    {
    public:
        virtual ~AdjustmentTool() = default;

        // Stable id, also the node's "tool" value in the document.
        const std::string &id() const { return descriptor().id; }
        ToolCategory category() const { return descriptor().category; }

        // Parameters, limits and defaults — handed to the UI verbatim.
        virtual const ToolDescriptor &descriptor() const = 0;

        // A fresh identity state (every parameter at its default).
        virtual std::unique_ptr<ToolState> defaultState() const = 0;

        // Parse this tool's params object. Returns null and sets `error`
        // when the params are malformed or their version is newer than this
        // build understands. Values outside the limits are an error, not
        // clamped: a document is either valid or it is refused.
        virtual std::unique_ptr<ToolState> parse(const nlohmann::json &params, std::string &error) const = 0;

        // This tool's params for `state` (includes "version").
        virtual nlohmann::json serialize(const ToolState &state) const = 0;

        // True when applying `state` changes nothing.
        virtual bool isIdentity(const ToolState &state) const = 0;

        // Semantic validation against the image the node receives (`input`
        // is the canvas after the nodes before it). Limits are checked in
        // parse(); this is for rules that need the image.
        virtual bool validate(const ToolState &state, Size input, std::string &error) const = 0;

        // Apply `state` to `io` in place. Geometry tools replace the pixels
        // with a new canvas; everything else keeps the canvas. The engine
        // never calls this for an identity state.
        virtual void apply(const ToolState &state, Frame &io, const RenderContext &ctx) const = 0;

        // The canvas after this tool, from the canvas it receives. Identity
        // for every non-geometry tool.
        virtual Size transformSize(const ToolState &state, Size input) const = 0;
    };
}
