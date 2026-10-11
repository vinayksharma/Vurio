#pragma once

// ADJ-1 / CHAIN-1 (docs/todo_tool_chain.md §5.6): what a tool tells the UI
// about itself. The server is the single owner of parameter limits and
// defaults; the client builds its controls from this and never hard-codes a
// range. The category decides how the engine composes the tool and where a
// node may be placed (§3.2).

#include <string>
#include <vector>

namespace MediaDedup::Adjustments
{
    struct ParamSpec
    {
        enum class Kind
        {
            Number,  // double within [min, max], stepped by `step` (0 = free)
            Integer, // integer within [min, max]
            Enum,    // one of `options`
            Bool,    // true/false; `def` != 0 means true
            Text     // free text (prompt masks); `default_option` is the default
        };

        std::string id;    // "angle"
        std::string label; // "Straighten"
        Kind kind = Kind::Number;
        double min = 0;
        double max = 0;
        double step = 0;
        double def = 0;                   // Number/Integer/Bool default
        std::string default_option;       // Enum/Text default
        std::vector<std::string> options; // Enum choices

        static const char *kindName(Kind k)
        {
            switch (k)
            {
            case Kind::Number: return "number";
            case Kind::Integer: return "integer";
            case Kind::Enum: return "enum";
            case Kind::Bool: return "bool";
            case Kind::Text: return "text";
            }
            return "number";
        }
    };

    enum class ToolCategory
    {
        Geometry, // changes the canvas; trunk only
        Raster,   // rewrites pixels, keeps the canvas; optional mix
        Layer,    // derived image blended onto its input
        Mask,     // coverage, lane mask slot only
        Split     // structural
    };

    inline const char *categoryName(ToolCategory c)
    {
        switch (c)
        {
        case ToolCategory::Geometry: return "geometry";
        case ToolCategory::Raster: return "raster";
        case ToolCategory::Layer: return "layer";
        case ToolCategory::Mask: return "mask";
        case ToolCategory::Split: return "split";
        }
        return "layer";
    }

    struct ToolDescriptor
    {
        std::string id;    // "rotate", "crop"
        std::string label; // "Rotate", "Crop"
        int version = 1;   // the params version this build writes
        ToolCategory category = ToolCategory::Layer;
        std::string icon;                   // SF Symbol name
        bool lane_allowed = true;           // false for geometry
        bool resolution_dependent = false;  // wants RenderContext::pixel_scale
        std::string summary;                // chain-row template, e.g. "{angle:+.1f}°"
        std::vector<ParamSpec> params;
    };
}
