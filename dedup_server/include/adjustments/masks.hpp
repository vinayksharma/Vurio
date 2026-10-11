#pragma once

// CHAIN-4 (docs/todo_tool_chain.md §5.6): procedural mask sources — the
// coverage a lane applies when it merges back onto the trunk. Geometric
// sources (linear, radial) are stored in BASE space and carried to the
// fork canvas by the frame's affine (docs §3.4); tonal sources (luminance,
// colour range) read the fork pixels. A lane's mask is a list of
// components combined left to right (add, subtract, intersect), then
// inverted and feathered. Detected sources (subject, people, faces,
// prompt) arrive with CHAIN-5 behind the same interface.

#include "adjustments/frame.hpp"
#include "adjustments/tools/param_tool.hpp"
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace MediaDedup::Adjustments
{
    // CHAIN-5: what a detected source needs to find its cached coverage —
    // the file the frame came from — and how to behave when it is not
    // there yet: a preview leaves the lane as a no-op and flags `pending`
    // (the detection is queued), a thumbnail or full render computes it
    // on the spot. Procedural sources ignore it.
    struct MaskContext
    {
        std::string file_path;
        bool inline_detection = true;
        mutable bool pending = false;
    };

    class MaskSource
    {
    public:
        virtual ~MaskSource() = default;
        const std::string &id() const { return descriptor_.id; }
        const ToolDescriptor &descriptor() const { return descriptor_; }
        std::unique_ptr<ToolState> parse(const nlohmann::json &params, std::string &error) const
        {
            return schema_.parse(params, error);
        }
        nlohmann::json serialize(const ToolState &state) const { return schema_.serialize(state); }
        std::unique_ptr<ToolState> defaultState() const { return schema_.defaultState(); }

        // Coverage in [0,1] (CV_32FC1, fork size) for `state` on `fork`.
        virtual void coverage(const ToolState &state, const Frame &fork, cv::Mat &out, const MaskContext &ctx) const = 0;
        // CHAIN-5: computed once per file by the MaskService, not per render.
        virtual bool detected() const { return false; }

    protected:
        MaskSource() : schema_(descriptor_) {}
        ToolDescriptor descriptor_;
        ParamSchema schema_;
        static const ParamState &as(const ToolState &s) { return static_cast<const ParamState &>(s); }
    };

    class MaskRegistry
    {
    public:
        static const MaskRegistry &instance();
        const std::vector<const MaskSource *> &sources() const { return order_; }
        const MaskSource *find(const std::string &id) const;
        std::vector<ToolDescriptor> descriptors() const;

    private:
        MaskRegistry();
        std::vector<std::unique_ptr<MaskSource>> owned_;
        std::vector<const MaskSource *> order_;
    };

    // One compiled lane mask: components, invert, feather.
    struct LaneMask
    {
        struct Component
        {
            const MaskSource *source = nullptr;
            std::unique_ptr<ToolState> state;
            std::string op = "add"; // add | subtract | intersect
        };
        std::vector<Component> components;
        bool invert = false;
        double feather = 0.0; // fraction of the base's shorter side

        LaneMask() = default;
        LaneMask(const LaneMask &other);
        LaneMask &operator=(const LaneMask &other);
        LaneMask(LaneMask &&) = default;
        LaneMask &operator=(LaneMask &&) = default;

        // Parse the document's "mask" object. Unknown sources are refused.
        static bool parse(const nlohmann::json &mask, LaneMask &out, std::string &error);
        // Coverage on the fork (CV_32FC1).
        void coverage(const Frame &fork, cv::Mat &out, const MaskContext &ctx = MaskContext()) const;
        // Any component that is a detected source.
        bool hasDetected() const;
    };

    namespace MaskMath
    {
        inline double smoothstep(double e0, double e1, double x)
        {
            if (e1 <= e0)
                return x >= e1 ? 1.0 : 0.0;
            double t = (x - e0) / (e1 - e0);
            t = t < 0 ? 0 : (t > 1 ? 1 : t);
            return t * t * (3 - 2 * t);
        }
    }
}
