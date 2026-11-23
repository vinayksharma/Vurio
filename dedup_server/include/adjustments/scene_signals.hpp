#pragma once

// LOOK-4 (docs/todo_film_looks.md): what a photo says about its own scene,
// as a handful of numbers a recipe can follow. The plain signals are in
// [0,1] and always available; the camera's own values (ISO, shutter,
// aperture, exposure value) are served as "advanced" signals and only when
// the file's metadata has them.
//
// Measured values come from a copy of the base no larger than kMeasureSide,
// so they are the same for every render size.

#include "adjustments/frame.hpp"
#include <functional>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace MediaDedup::Adjustments
{
    struct SignalSpec
    {
        std::string id;         // "scene_brightness"
        std::string label;      // "Scene brightness"
        std::string low_label;  // "Dark"
        std::string high_label; // "Bright"
        double min = 0, max = 1;
        bool advanced = false;  // a raw camera value
    };

    struct SceneSignals
    {
        static constexpr int kMeasureSide = 256;
        static constexpr const char *kSceneBrightness = "scene_brightness";
        static constexpr const char *kLightColour = "light_colour";
        static constexpr const char *kNoise = "noise";
        static constexpr const char *kHighlights = "highlights";
        static constexpr const char *kIso = "iso";
        static constexpr const char *kShutter = "shutter";
        static constexpr const char *kAperture = "aperture";
        static constexpr const char *kExposureValue = "exposure_value";

        // What the camera recorded; any of it may be missing.
        struct Exposure
        {
            std::optional<double> iso;
            std::optional<double> shutter_seconds;
            std::optional<double> aperture;
        };

        std::map<std::string, double> values; // only the signals this photo has

        static const std::vector<SignalSpec> &catalogue();
        static const SignalSpec *find(const std::string &id);

        bool has(const std::string &id) const { return values.count(id) != 0; }
        double value(const std::string &id, double fallback = 0.0) const
        {
            auto it = values.find(id);
            return it == values.end() ? fallback : it->second;
        }

        // The signals of a picture (8-bit BGR, any size) and its exposure.
        //
        //   scene_brightness: with ISO, shutter and aperture, the scene's
        //     exposure value EV100 = log2(N²/t) − log2(ISO/100), moved by how
        //     far the picture's mean linear luma sits from mid-grey (± 3 EV
        //     at most), mapped −2 EV → 0, 14 EV → 1. Without them, the
        //     picture alone: 0.5 + log2(mean/0.18)/8.
        //   light_colour: 0.5 + 0.25·log2(mean R / mean B), linear light.
        //   noise: log2(ISO/100)/7 with an ISO, else the measured noise
        //     sigma (Immerkær) over 0.04.
        //   highlights: the share of pixels with a channel ≥ 250, over 5 %.
        static SceneSignals measure(const cv::Mat &bgr8, const Exposure &exposure);

        nlohmann::json toJson() const;
    };

    // Signals per file, measured once and remembered.
    class SignalService
    {
    public:
        using BaseLoader = std::function<cv::Mat(const std::string &file_path, int max_side, Size &base_full)>;
        using ExposureLookup = std::function<SceneSignals::Exposure(const std::string &file_path)>;

        static SignalService &instance();
        void configure(BaseLoader loader, ExposureLookup exposure);
        void reset();

        // Empty `values` when the file cannot be read and has no metadata.
        SceneSignals forFile(const std::string &file_path);
        void forget(const std::string &file_path);

    private:
        static constexpr size_t kCacheEntries = 512;
        std::mutex mutex_;
        BaseLoader loader_;
        ExposureLookup exposure_;
        std::map<std::string, SceneSignals> cache_;
    };
}
