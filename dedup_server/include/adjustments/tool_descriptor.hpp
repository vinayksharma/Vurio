#pragma once

// ADJ-1: what a tool tells the UI about itself. The server is the single
// owner of parameter limits and defaults; the client builds its controls
// from this and never hard-codes a range.

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
            Enum     // one of `options`
        };

        std::string id;    // "angle"
        std::string label; // "Straighten"
        Kind kind = Kind::Number;
        double min = 0;
        double max = 0;
        double step = 0;
        double def = 0;                   // Number/Integer default
        std::string default_option;       // Enum default
        std::vector<std::string> options; // Enum choices
    };

    struct ToolDescriptor
    {
        std::string id;    // "rotate", "crop"
        std::string label; // "Rotate", "Crop"
        int version = 1;   // the section version this build writes
        std::vector<ParamSpec> params;
    };
}
