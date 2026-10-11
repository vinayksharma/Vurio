#include "adjustments/chain_engine.hpp"
#include <Poco/Logger.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace MediaDedup::Adjustments::ChainEngine
{
    namespace
    {
        Poco::Logger &log() { return Poco::Logger::get("ChainEngine"); }
    }

    Frame fromBgr8(const cv::Mat &bgr8)
    {
        Frame f;
        if (bgr8.empty())
            return f;
        cv::Mat src = bgr8;
        if (src.channels() == 1)
            cv::cvtColor(src, src, cv::COLOR_GRAY2BGR);
        else if (src.channels() == 4)
            cv::cvtColor(src, src, cv::COLOR_BGRA2BGR);
        if (src.depth() == CV_16U)
            src.convertTo(f.pixels, CV_32FC3, 1.0 / 65535.0);
        else
            src.convertTo(f.pixels, CV_32FC3, 1.0 / 255.0);
        f.resetAffine();
        return f;
    }

    cv::Mat toBgr8(const Frame &frame)
    {
        cv::Mat out;
        if (frame.empty())
            return out;
        frame.pixels.convertTo(out, CV_8UC3, 255.0); // rounds and saturates
        return out;
    }

    cv::Mat decodeBgr8(const std::string &path, int max_side)
    {
        try
        {
            cv::Mat img;
            if (max_side > 0)
            {
                // libjpeg's scaled decode is far cheaper than a full decode
                // plus a resize; take it when the result still covers the
                // working size.
                img = cv::imread(path, cv::IMREAD_REDUCED_COLOR_2);
                if (!img.empty() && std::max(img.rows, img.cols) < max_side)
                    img = cv::Mat();
            }
            if (img.empty())
                img = cv::imread(path, cv::IMREAD_COLOR);
            return img;
        }
        catch (const std::exception &e)
        {
            log().warning("decode failed for %s: %s", path, std::string(e.what()));
            return {};
        }
    }

    cv::Mat decodeBgr8(const std::vector<uint8_t> &encoded)
    {
        try
        {
            return cv::imdecode(encoded, cv::IMREAD_COLOR);
        }
        catch (const std::exception &e)
        {
            log().warning("decode of in-memory image failed: %s", std::string(e.what()));
            return {};
        }
    }

    double workingScale(const RenderPlan &plan, Size base, int target)
    {
        if (target <= 0 || base.width <= 0 || base.height <= 0)
            return 1.0;
        Size out = plan.displaySize(base);
        double longest = std::max(out.width, out.height);
        if (longest <= target)
            return 1.0;
        return static_cast<double>(target) / longest;
    }

    void scaleFrame(Frame &frame, double scale)
    {
        if (frame.empty() || scale <= 0 || std::fabs(scale - 1.0) < 1e-9)
            return;
        int nw = std::max(1, static_cast<int>(std::lround(frame.width() * scale)));
        int nh = std::max(1, static_cast<int>(std::lround(frame.height() * scale)));
        if (nw == frame.width() && nh == frame.height())
            return;
        cv::Mat out;
        cv::resize(frame.pixels, out, cv::Size(nw, nh), 0, 0, scale < 1.0 ? cv::INTER_AREA : cv::INTER_LINEAR);
        double sx = static_cast<double>(nw) / frame.width(), sy = static_cast<double>(nh) / frame.height();
        frame.pixels = out;
        frame.composeAffine(cv::Matx23d(sx, 0, 0, 0, sy, 0));
    }

    void fitFrame(Frame &frame, int max_side)
    {
        if (frame.empty() || max_side <= 0)
            return;
        int longest = std::max(frame.width(), frame.height());
        if (longest <= max_side)
            return;
        scaleFrame(frame, static_cast<double>(max_side) / longest);
    }

    cv::Mat gaussianBlur(const cv::Mat &pixels, double sigma_px)
    {
        cv::Mat out;
        if (pixels.empty() || sigma_px <= 0)
            return pixels.clone();
        if (sigma_px <= 4.0)
        {
            cv::GaussianBlur(pixels, out, cv::Size(0, 0), sigma_px, sigma_px, cv::BORDER_REFLECT);
            return out;
        }
        double factor = sigma_px / 2.0; // blur at sigma 2 on the reduced level
        int w = std::max(1, static_cast<int>(std::lround(pixels.cols / factor)));
        int h = std::max(1, static_cast<int>(std::lround(pixels.rows / factor)));
        cv::Mat small, blurred;
        cv::resize(pixels, small, cv::Size(w, h), 0, 0, cv::INTER_AREA);
        cv::GaussianBlur(small, blurred, cv::Size(0, 0), 2.0, 2.0, cv::BORDER_REFLECT);
        cv::resize(blurred, out, pixels.size(), 0, 0, cv::INTER_LINEAR);
        return out;
    }

    std::vector<uint8_t> encodeJpeg(const Frame &frame, int quality)
    {
        std::vector<uint8_t> out;
        if (frame.empty())
            return out;
        try
        {
            cv::Mat bgr8 = toBgr8(frame);
            if (!cv::imencode(".jpg", bgr8, out, {cv::IMWRITE_JPEG_QUALITY, std::clamp(quality, 1, 100)}))
                out.clear();
        }
        catch (const std::exception &e)
        {
            log().error("JPEG encode failed: %s", std::string(e.what()));
            out.clear();
        }
        return out;
    }

    bool renderToJpeg(const cv::Mat &base_bgr8, const RenderPlan &plan, int target, int quality,
                      std::vector<uint8_t> &jpeg_out, int *out_width, int *out_height,
                      const RenderPlan::RenderOptions &options)
    {
        if (base_bgr8.empty())
        {
            log().error("renderToJpeg: empty base image");
            return false;
        }
        Frame frame = fromBgr8(base_bgr8);
        double scale = workingScale(plan, frame.size(), target);
        scaleFrame(frame, scale);
        plan.render(frame, 0, nullptr, options);
        if (target > 0)
            fitFrame(frame, target);
        jpeg_out = encodeJpeg(frame, quality);
        if (jpeg_out.empty())
            return false;
        if (out_width)
            *out_width = frame.width();
        if (out_height)
            *out_height = frame.height();
        return true;
    }
}
