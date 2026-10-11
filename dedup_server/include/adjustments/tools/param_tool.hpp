#pragma once

// CHAIN-2 (docs/todo_tool_chain.md §6): the common half of every tool whose
// state is just its descriptor's parameters. Parsing, serialising, limits,
// identity and the (unchanged) canvas are handled here from the ParamSpecs;
// a concrete tool declares its params in its constructor and implements
// `apply`. Rotate and Crop keep their own state classes because their
// params have structure (a rectangle, an aspect lock) this does not model.

#include "adjustments/adjustment_tool.hpp"
#include <map>
#include <string>

namespace MediaDedup::Adjustments
{
    struct ParamState final : ToolState
    {
        std::map<std::string, double> numbers;      // Number, Integer, Bool (0/1)
        std::map<std::string, std::string> strings; // Enum, Text
        std::unique_ptr<ToolState> clone() const override { return std::make_unique<ParamState>(*this); }

        double number(const std::string &id, double def = 0.0) const
        {
            auto it = numbers.find(id);
            return it == numbers.end() ? def : it->second;
        }
        const std::string &string(const std::string &id) const
        {
            static const std::string empty;
            auto it = strings.find(id);
            return it == strings.end() ? empty : it->second;
        }
    };

    class ParamTool : public AdjustmentTool
    {
    public:
        const ToolDescriptor &descriptor() const override { return descriptor_; }
        std::unique_ptr<ToolState> defaultState() const override;
        std::unique_ptr<ToolState> parse(const nlohmann::json &params, std::string &error) const override;
        nlohmann::json serialize(const ToolState &state) const override;
        // Every parameter at its default.
        bool isIdentity(const ToolState &state) const override;
        bool validate(const ToolState &, Size, std::string &) const override { return true; }
        Size transformSize(const ToolState &, Size input) const override { return input; }

    protected:
        ToolDescriptor descriptor_;
        static const ParamState &as(const ToolState &s) { return static_cast<const ParamState &>(s); }

        // Builders for the constructor.
        void number(const char *id, const char *label, double min, double max, double def, double step = 0.0);
        void integer(const char *id, const char *label, int min, int max, int def);
        void choice(const char *id, const char *label, std::vector<std::string> options, const char *def);
        void flag(const char *id, const char *label, bool def);
        void text(const char *id, const char *label, const char *def);
    };
}
