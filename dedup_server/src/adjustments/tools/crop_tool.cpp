#include "adjustments/tools/crop_tool.hpp"
#include <algorithm>
#include <cmath>

namespace MediaDedup::Adjustments
{
    const std::vector<std::string> CropTool::kAspectOptions = {
        "free", "original", "1:1", "3:2", "4:3", "5:4", "16:9", "custom"};

    namespace
    {
        const CropState &as(const ToolState &s) { return static_cast<const CropState &>(s); }

        bool readUnit(const nlohmann::json &section, const char *key, double &out, std::string &error)
        {
            if (!section.contains(key))
                return true;
            if (!section[key].is_number())
            {
                error = std::string("crop: ") + key + " must be a number";
                return false;
            }
            out = section[key].get<double>();
            if (!std::isfinite(out) || out < 0.0 || out > 1.0)
            {
                error = std::string("crop: ") + key + " must be within [0, 1]";
                return false;
            }
            return true;
        }
    }

    CropTool::CropTool()
    {
        descriptor_.id = kId;
        descriptor_.label = "Crop";
        descriptor_.version = kVersion;
        descriptor_.category = ToolCategory::Geometry;
        descriptor_.icon = "crop";
        descriptor_.lane_allowed = false;
        descriptor_.summary = "{aspect}";

        for (const char *id : {"left", "top", "width", "height"})
        {
            ParamSpec p;
            p.id = id;
            p.label = id;
            p.kind = ParamSpec::Kind::Number;
            p.min = 0.0;
            p.max = 1.0;
            p.step = 0.0;
            p.def = (std::string(id) == "width" || std::string(id) == "height") ? 1.0 : 0.0;
            descriptor_.params.push_back(p);
        }

        ParamSpec aspect;
        aspect.id = "aspect";
        aspect.label = "Aspect";
        aspect.kind = ParamSpec::Kind::Enum;
        aspect.options = kAspectOptions;
        aspect.default_option = "free";
        descriptor_.params.push_back(aspect);

        ParamSpec custom;
        custom.id = "custom_aspect";
        custom.label = "Custom aspect (w/h)";
        custom.kind = ParamSpec::Kind::Number;
        custom.min = kMinCustomAspect;
        custom.max = kMaxCustomAspect;
        custom.step = 0.01;
        custom.def = 1.0;
        descriptor_.params.push_back(custom);
    }

    std::unique_ptr<ToolState> CropTool::defaultState() const
    {
        return std::make_unique<CropState>();
    }

    std::unique_ptr<ToolState> CropTool::parse(const nlohmann::json &section, std::string &error) const
    {
        if (!section.is_object())
        {
            error = "crop: section is not an object";
            return nullptr;
        }
        int version = section.value("version", 1);
        if (version > kVersion)
        {
            error = "crop: section version " + std::to_string(version) + " is newer than supported " +
                    std::to_string(kVersion);
            return nullptr;
        }
        auto state = std::make_unique<CropState>();
        if (!readUnit(section, "left", state->rect.left, error) ||
            !readUnit(section, "top", state->rect.top, error) ||
            !readUnit(section, "width", state->rect.width, error) ||
            !readUnit(section, "height", state->rect.height, error))
            return nullptr;

        if (state->rect.width < kMinExtent || state->rect.height < kMinExtent)
        {
            error = "crop: width and height must be at least 1% of the image";
            return nullptr;
        }
        if (state->rect.right() > 1.0 + 1e-9 || state->rect.bottom() > 1.0 + 1e-9)
        {
            error = "crop: rectangle extends past the canvas";
            return nullptr;
        }
        if (section.contains("aspect"))
        {
            if (!section["aspect"].is_string())
            {
                error = "crop: aspect must be a string";
                return nullptr;
            }
            state->aspect = section["aspect"].get<std::string>();
            if (std::find(kAspectOptions.begin(), kAspectOptions.end(), state->aspect) == kAspectOptions.end())
            {
                error = "crop: unknown aspect '" + state->aspect + "'";
                return nullptr;
            }
        }
        if (section.contains("custom_aspect"))
        {
            if (!section["custom_aspect"].is_number())
            {
                error = "crop: custom_aspect must be a number";
                return nullptr;
            }
            state->custom_aspect = section["custom_aspect"].get<double>();
            if (!std::isfinite(state->custom_aspect) || state->custom_aspect < kMinCustomAspect ||
                state->custom_aspect > kMaxCustomAspect)
            {
                error = "crop: custom_aspect must be within [0.1, 10]";
                return nullptr;
            }
        }
        return state;
    }

    nlohmann::json CropTool::serialize(const ToolState &state) const
    {
        const CropState &s = as(state);
        nlohmann::json j;
        j["version"] = kVersion;
        j["left"] = s.rect.left;
        j["top"] = s.rect.top;
        j["width"] = s.rect.width;
        j["height"] = s.rect.height;
        j["aspect"] = s.aspect;
        if (s.aspect == "custom")
            j["custom_aspect"] = s.custom_aspect;
        return j;
    }

    bool CropTool::isIdentity(const ToolState &state) const
    {
        // The aspect lock alone changes no pixels.
        return as(state).rect.isFull();
    }

    bool CropTool::validate(const ToolState &state, Size input, std::string &error) const
    {
        const CropState &s = as(state);
        if (input.width > 0 && input.height > 0)
        {
            Size px = cropPixels(input, s.rect);
            if (px.width < 1 || px.height < 1)
            {
                error = "crop: rectangle is smaller than one pixel";
                return false;
            }
        }
        return true;
    }

    cv::Rect CropTool::pixelRect(const NormRect &rect, int width, int height)
    {
        Size px = cropPixels({static_cast<double>(width), static_cast<double>(height)}, rect);
        int w = static_cast<int>(px.width);
        int h = static_cast<int>(px.height);
        int x = static_cast<int>(std::lround(rect.left * width));
        int y = static_cast<int>(std::lround(rect.top * height));
        x = std::clamp(x, 0, std::max(0, width - 1));
        y = std::clamp(y, 0, std::max(0, height - 1));
        w = std::clamp(w, 1, width - x);
        h = std::clamp(h, 1, height - y);
        return cv::Rect(x, y, w, h);
    }

    void CropTool::apply(const ToolState &state, Frame &io, const RenderContext &) const
    {
        const CropState &s = as(state);
        if (isIdentity(s) || io.empty())
            return;
        cv::Rect roi = pixelRect(s.rect, io.width(), io.height());
        // A copy, not a view: later nodes and the node cache expect a
        // contiguous frame that owns its pixels.
        io.pixels = io.pixels(roi).clone();
    }

    Size CropTool::transformSize(const ToolState &state, Size input) const
    {
        const CropState &s = as(state);
        if (input.width <= 0 || input.height <= 0)
            return input;
        return cropPixels(input, s.rect);
    }
}
