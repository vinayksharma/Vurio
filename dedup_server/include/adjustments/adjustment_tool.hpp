#pragma once

// ADJ-1 (docs/todo_image_adjustments.md §5.2): the interface every
// adjustment tool implements. A tool is a self-contained unit: it owns its
// parameters and limits (descriptor), its JSON section (parse/serialize),
// its validation, and its application to an image — one pixel backend (the
// ImageMagick argument chain, §5.4) and one geometry backend (display
// dimensions). The application never names a concrete tool outside the
// registry.

#include "adjustments/adjustment_geometry.hpp"
#include "adjustments/tool_descriptor.hpp"
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace MediaDedup::Adjustments
{
    // A tool's parameter values for one file. Each tool defines its own
    // concrete state; the chain holds them behind this base.
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

        // Stable id, also the key of the tool's section in the document.
        const std::string &id() const { return descriptor().id; }

        // Parameters, limits and defaults — handed to the UI verbatim.
        virtual const ToolDescriptor &descriptor() const = 0;

        // A fresh identity state (every parameter at its default).
        virtual std::unique_ptr<ToolState> defaultState() const = 0;

        // Parse this tool's JSON section. Returns null and sets `error` when
        // the section is malformed or its version is newer than this build
        // understands. Values outside the limits are an error, not clamped:
        // a document is either valid or it is refused.
        virtual std::unique_ptr<ToolState> parse(const nlohmann::json &section,
                                                 std::string &error) const = 0;

        // This tool's JSON section for `state` (includes "version").
        virtual nlohmann::json serialize(const ToolState &state) const = 0;

        // True when applying `state` changes nothing.
        virtual bool isIdentity(const ToolState &state) const = 0;

        // Semantic validation against the image the chain is applied to
        // (`input` is the size this tool receives, i.e. after the tools
        // before it). Limits are checked in parse(); this is for rules that
        // need the image, such as "the crop lies inside the rotated content".
        virtual bool validate(const ToolState &state, Size input, std::string &error) const = 0;

        // Append this tool's ImageMagick operators to `args`. Called between
        // -auto-orient and -colorspace; -resize follows later, so operators
        // see the full-size image. Every geometry operator must be followed
        // by +repage (ThumbnailGenerator's resize fits against the virtual
        // canvas otherwise).
        virtual void appendMagickOps(const ToolState &state, std::vector<std::string> &args) const = 0;

        // The image size after this tool, from the size it receives.
        virtual Size transformSize(const ToolState &state, Size input) const = 0;
    };
}
