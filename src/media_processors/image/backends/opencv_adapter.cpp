#include "media_processors/image/backends/opencv_adapter.hpp"
#include <Poco/Logger.h>

#ifdef HAVE_OPENCV
#include <opencv2/opencv.hpp>
#include <opencv2/img_hash.hpp>
#endif

namespace MediaDedup
{
    bool OpenCvAdapter::ComputePhash(const std::string &file_path,
                                     int thumb_size,
                                     OpenCvHashResult &out)
    {
#ifdef HAVE_OPENCV
        try
        {
            // Disable internal threading to let TPM control concurrency
            cv::setNumThreads(0);

            cv::Mat img = cv::imread(file_path, cv::IMREAD_COLOR);
            if (img.empty())
            {
                Poco::Logger::get("OpenCvAdapter").warning("Failed to load image: %s", file_path);
                return false;
            }
            int w = img.cols, h = img.rows;
            if (w == 0 || h == 0)
                return false;
            // Maintain aspect ratio, fit within thumb_size
            int new_w = w, new_h = h;
            if (w > h)
            {
                new_w = thumb_size;
                new_h = static_cast<int>((static_cast<double>(h) / w) * thumb_size);
            }
            else
            {
                new_h = thumb_size;
                new_w = static_cast<int>((static_cast<double>(w) / h) * thumb_size);
            }
            if (new_w <= 0)
                new_w = 1;
            if (new_h <= 0)
                new_h = 1;
            cv::Mat resized;
            cv::resize(img, resized, cv::Size(new_w, new_h), 0, 0, cv::INTER_AREA);
            // Compute perceptual hash (pHash)
            cv::Mat hash;
            cv::img_hash::pHash(resized, hash);
            // Reduce to 64-bit equivalent by hashing the cv::Mat bytes (simple approach)
            std::uint64_t acc = 1469598103934665603ULL; // FNV offset basis
            for (int r = 0; r < hash.rows; ++r)
            {
                const unsigned char *ptr = hash.ptr<unsigned char>(r);
                for (int c = 0; c < hash.cols; ++c)
                {
                    acc ^= static_cast<std::uint64_t>(ptr[c]);
                    acc *= 1099511628211ULL; // FNV prime
                }
            }
            out.phash64.resize(8);
            for (int i = 0; i < 8; ++i)
                out.phash64[i] = static_cast<std::uint8_t>((acc >> (i * 8)) & 0xFF);
            out.thumb_w = new_w;
            out.thumb_h = new_h;
            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("OpenCvAdapter").error("OpenCV exception: %s", std::string(e.what()));
            return false;
        }
#else
        (void)file_path;
        (void)thumb_size;
        (void)out;
        return false;
#endif
    }

    bool OpenCvAdapter::ComputePhash(const std::vector<std::uint8_t> &image_data,
                                     int thumb_size,
                                     OpenCvHashResult &out)
    {
#ifdef HAVE_OPENCV
        try
        {
            // Disable internal threading to let TPM control concurrency
            cv::setNumThreads(0);

            // Decode image from memory buffer
            cv::Mat img = cv::imdecode(image_data, cv::IMREAD_COLOR);
            if (img.empty())
            {
                Poco::Logger::get("OpenCvAdapter").warning("Failed to decode image from memory data");
                return false;
            }

            int w = img.cols, h = img.rows;
            if (w == 0 || h == 0)
                return false;

            // Maintain aspect ratio, fit within thumb_size
            int new_w = w, new_h = h;
            if (w > h)
            {
                new_w = thumb_size;
                new_h = static_cast<int>((static_cast<double>(h) / w) * thumb_size);
            }
            else
            {
                new_h = thumb_size;
                new_w = static_cast<int>((static_cast<double>(w) / h) * thumb_size);
            }
            if (new_w <= 0)
                new_w = 1;
            if (new_h <= 0)
                new_h = 1;

            cv::Mat resized;
            cv::resize(img, resized, cv::Size(new_w, new_h), 0, 0, cv::INTER_AREA);

            // Compute perceptual hash (pHash)
            cv::Mat hash;
            cv::img_hash::pHash(resized, hash);

            // Reduce to 64-bit equivalent by hashing the cv::Mat bytes (simple approach)
            std::uint64_t acc = 1469598103934665603ULL; // FNV offset basis
            for (int r = 0; r < hash.rows; ++r)
            {
                for (int c = 0; c < hash.cols; ++c)
                {
                    acc ^= static_cast<std::uint8_t>(hash.at<uchar>(r, c));
                    acc *= 1099511628211ULL; // FNV prime
                }
            }

            out.phash64.resize(8);
            for (int i = 0; i < 8; ++i)
            {
                out.phash64[i] = static_cast<std::uint8_t>((acc >> (i * 8)) & 0xFF);
            }
            out.thumb_w = new_w;
            out.thumb_h = new_h;
            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("OpenCvAdapter").error("OpenCV memory processing exception: %s", std::string(e.what()));
            return false;
        }
#else
        (void)image_data;
        (void)thumb_size;
        (void)out;
        return false;
#endif
    }
}
