#include "media_processors/image/backends/features_adapter.hpp"
#include <Poco/Logger.h>

#ifdef HAVE_OPENCV
#include <opencv2/opencv.hpp>
#include <algorithm>
#endif

namespace MediaDedup
{
    static void appendBytes(std::vector<std::uint8_t> &buf, const void *data, size_t len)
    {
        const auto *p = static_cast<const std::uint8_t *>(data);
        buf.insert(buf.end(), p, p + len);
    }

    static void appendU32(std::vector<std::uint8_t> &buf, std::uint32_t v)
    {
        appendBytes(buf, &v, sizeof(v));
    }

    bool FeaturesAdapter::ExtractFeaturesToBlob(const std::string &file_path,
                                                int resize_long_edge,
                                                int max_keypoints,
                                                std::vector<std::uint8_t> &out_blob)
    {
#ifdef HAVE_OPENCV
        try
        {
            cv::setNumThreads(0);
            cv::Mat img = cv::imread(file_path, cv::IMREAD_GRAYSCALE);
            if (img.empty())
                return false;

            int w = img.cols, h = img.rows;
            if (w == 0 || h == 0)
                return false;
            int new_w = w, new_h = h;
            if (w > h)
            {
                new_w = resize_long_edge;
                new_h = static_cast<int>((static_cast<double>(h) / w) * resize_long_edge);
            }
            else
            {
                new_h = resize_long_edge;
                new_w = static_cast<int>((static_cast<double>(w) / h) * resize_long_edge);
            }
            if (new_w <= 0)
                new_w = 1;
            if (new_h <= 0)
                new_h = 1;
            cv::Mat resized;
            cv::resize(img, resized, cv::Size(new_w, new_h), 0, 0, cv::INTER_AREA);

            // ORB for speed and availability (no nonfree modules)
            auto detector = cv::ORB::create(max_keypoints);
            std::vector<cv::KeyPoint> kps;
            cv::Mat desc;
            detector->detectAndCompute(resized, cv::noArray(), kps, desc);

            if (kps.empty() || desc.empty())
            {
                out_blob.clear();
                appendU32(out_blob, 0);
                appendU32(out_blob, 0);
                appendU32(out_blob, 0);
                appendU32(out_blob, 0);
                return true; // empty features are valid
            }

            // Cap keypoints to max_keypoints and rows accordingly
            if (static_cast<int>(kps.size()) > max_keypoints)
            {
                kps.resize(max_keypoints);
                desc = desc.rowRange(0, static_cast<int>(kps.size())).clone();
            }

            out_blob.clear();
            std::uint32_t num_kp = static_cast<std::uint32_t>(kps.size());
            std::uint32_t drows = static_cast<std::uint32_t>(desc.rows);
            std::uint32_t dcols = static_cast<std::uint32_t>(desc.cols);
            std::uint32_t dtype = static_cast<std::uint32_t>(desc.type());
            appendU32(out_blob, num_kp);
            appendU32(out_blob, drows);
            appendU32(out_blob, dcols);
            appendU32(out_blob, dtype);

            for (const auto &kp : kps)
            {
                float vals[6] = {kp.pt.x, kp.pt.y, kp.size, kp.angle, kp.response, static_cast<float>(kp.octave)};
                appendBytes(out_blob, vals, sizeof(vals));
                std::int32_t cls = kp.class_id;
                appendBytes(out_blob, &cls, sizeof(cls));
            }

            size_t bytes = desc.total() * desc.elemSize();
            appendBytes(out_blob, desc.data, bytes);
            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("FeaturesAdapter").error("OpenCV exception: %s", std::string(e.what()));
            return false;
        }
#else
        (void)file_path;
        (void)resize_long_edge;
        (void)max_keypoints;
        (void)out_blob;
        return false;
#endif
    }
}


