#include "adjustments/scene_signals.hpp"
#include "adjustments/blend.hpp"
#include <algorithm>
#include <cmath>
#include <opencv2/imgproc.hpp>

namespace MediaDedup::Adjustments
{
    namespace
    {
        double clamp01(double v) { return std::min(1.0, std::max(0.0, v)); }
    }

    const std::vector<SignalSpec> &SceneSignals::catalogue()
    {
        static const std::vector<SignalSpec> specs = {
            {kSceneBrightness, "Scene brightness", "Dark", "Bright", 0, 1, false},
            {kLightColour, "Light colour", "Cool", "Warm", 0, 1, false},
            {kNoise, "Noise", "Clean", "Noisy", 0, 1, false},
            {kHighlights, "Clipped highlights", "None", "Many", 0, 1, false},
            {kIso, "ISO", "50", "102400", 50, 102400, true},
            {kShutter, "Shutter (seconds)", "1/8000", "30", 1.0 / 8000, 30, true},
            {kAperture, "Aperture (f-number)", "f/1", "f/32", 1, 32, true},
            {kExposureValue, "Exposure value (EV100)", "-6", "20", -6, 20, true},
        };
        return specs;
    }

    const SignalSpec *SceneSignals::find(const std::string &id)
    {
        for (const SignalSpec &s : catalogue())
            if (s.id == id)
                return &s;
        return nullptr;
    }

    SceneSignals SceneSignals::measure(const cv::Mat &bgr8, const Exposure &e)
    {
        SceneSignals out;
        const bool has_iso = e.iso && *e.iso > 0;
        const bool has_ev = has_iso && e.shutter_seconds && *e.shutter_seconds > 0 && e.aperture && *e.aperture > 0;
        if (has_iso)
            out.values[kIso] = *e.iso;
        if (e.shutter_seconds && *e.shutter_seconds > 0)
            out.values[kShutter] = *e.shutter_seconds;
        if (e.aperture && *e.aperture > 0)
            out.values[kAperture] = *e.aperture;
        double ev100 = 0;
        if (has_ev)
        {
            ev100 = std::log2(*e.aperture * *e.aperture / *e.shutter_seconds) - std::log2(*e.iso / 100.0);
            out.values[kExposureValue] = ev100;
        }
        if (has_iso)
            out.values[kNoise] = clamp01(std::log2(*e.iso / 100.0) / 7.0);

        if (bgr8.empty() || bgr8.type() != CV_8UC3)
        {
            if (has_ev)
                out.values[kSceneBrightness] = clamp01((ev100 + 2.0) / 16.0);
            return out;
        }
        cv::Mat small = bgr8;
        const int longest = std::max(bgr8.cols, bgr8.rows);
        if (longest > kMeasureSide)
        {
            double k = static_cast<double>(kMeasureSide) / longest;
            cv::resize(bgr8, small, cv::Size(std::max(1, static_cast<int>(std::lround(bgr8.cols * k))),
                                             std::max(1, static_cast<int>(std::lround(bgr8.rows * k)))),
                       0, 0, cv::INTER_AREA);
        }
        cv::Mat gamma, linear;
        small.convertTo(gamma, CV_32FC3, 1.0 / 255.0);
        linear = gamma.clone();
        Blend::toLinear(linear);
        const cv::Scalar mean = cv::mean(linear); // B, G, R
        const double luma = 0.0722 * mean[0] + 0.7152 * mean[1] + 0.2126 * mean[2];
        const double offset = std::log2(std::max(luma, 0.002) / 0.18);
        out.values[kSceneBrightness] = has_ev ? clamp01((ev100 + std::min(3.0, std::max(-3.0, offset)) + 2.0) / 16.0)
                                              : clamp01(0.5 + offset / 8.0);
        out.values[kLightColour] = clamp01(0.5 + 0.25 * std::log2(std::max(mean[2], 1e-4) / std::max(mean[0], 1e-4)));

        cv::Mat peak;
        std::vector<cv::Mat> ch;
        cv::split(small, ch);
        cv::max(ch[0], ch[1], peak);
        cv::max(peak, ch[2], peak);
        const double clipped = static_cast<double>(cv::countNonZero(peak >= 250)) / static_cast<double>(peak.total());
        out.values[kHighlights] = clamp01(clipped / 0.05);

        if (!has_iso && small.cols > 4 && small.rows > 4)
        {
            // Immerkær: sigma = sqrt(pi/2) / 6 · mean |L * [1 −2 1; −2 4 −2; 1 −2 1]|
            cv::Mat l, response;
            cv::transform(gamma, l, cv::Matx13f(0.0722f, 0.7152f, 0.2126f));
            const cv::Mat kernel = (cv::Mat_<float>(3, 3) << 1, -2, 1, -2, 4, -2, 1, -2, 1);
            cv::filter2D(l, response, CV_32F, kernel, cv::Point(-1, -1), 0, cv::BORDER_REFLECT);
            const double sigma = std::sqrt(CV_PI / 2.0) / 6.0 * cv::mean(cv::abs(response))[0];
            out.values[kNoise] = clamp01(sigma / 0.04);
        }
        return out;
    }

    nlohmann::json SceneSignals::toJson() const
    {
        nlohmann::json j = nlohmann::json::object();
        for (const auto &kv : values)
            j[kv.first] = kv.second;
        return j;
    }

    SignalService &SignalService::instance()
    {
        static SignalService service;
        return service;
    }

    void SignalService::configure(BaseLoader loader, ExposureLookup exposure, Provisional provisional)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        loader_ = std::move(loader);
        exposure_ = std::move(exposure);
        provisional_ = std::move(provisional);
        cache_.clear();
    }

    void SignalService::reset()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        loader_ = nullptr;
        exposure_ = nullptr;
        provisional_ = nullptr;
        cache_.clear();
    }

    void SignalService::forget(const std::string &file_path)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_.erase(file_path);
    }

    SceneSignals SignalService::forFile(const std::string &file_path)
    {
        BaseLoader loader;
        ExposureLookup exposure;
        Provisional provisional;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = cache_.find(file_path);
            if (it != cache_.end())
                return it->second;
            loader = loader_;
            exposure = exposure_;
            provisional = provisional_;
        }
        if (file_path.empty())
            return SceneSignals();
        // Asked before the decode: a base that turns real meanwhile is
        // measured again next time, never remembered as the stand-in.
        const bool stand_in = provisional && provisional(file_path);
        Size full;
        cv::Mat base = loader ? loader(file_path, SceneSignals::kMeasureSide * 2, full) : cv::Mat();
        SceneSignals s = SceneSignals::measure(base, exposure ? exposure(file_path) : SceneSignals::Exposure());
        // An unreadable file is not remembered: it may be readable later.
        if (!base.empty() && !stand_in)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (cache_.size() >= kCacheEntries)
                cache_.clear();
            cache_[file_path] = s;
        }
        return s;
    }
}
