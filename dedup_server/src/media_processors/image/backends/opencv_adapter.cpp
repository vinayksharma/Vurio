#include "media_processors/image/backends/opencv_adapter.hpp"
#include <Poco/Logger.h>

#ifdef HAVE_OPENCV
#include <opencv2/opencv.hpp>
#include <algorithm>
#include <vector>
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

            // Release original image memory immediately (can be 70MB+ for large images)
            img.release();

            // Compute proper DCT-based perceptual hash (64-bit)
            // Algorithm:
            // 1. Convert to grayscale
            // 2. Resize to 32x32
            // 3. Apply DCT
            // 4. Extract top-left 8x8 DCT coefficients (low frequencies)
            // 5. Compare to median to generate 64-bit hash

            cv::Mat gray;
            cv::cvtColor(resized, gray, cv::COLOR_BGR2GRAY);

            // Resize to 32x32 for DCT
            cv::Mat small;
            cv::resize(gray, small, cv::Size(32, 32), 0, 0, cv::INTER_AREA);

            // Release previous matrices
            resized.release();
            gray.release();

            // Convert to float for DCT
            cv::Mat float_img;
            small.convertTo(float_img, CV_32F);
            small.release();

            // Apply DCT (Discrete Cosine Transform)
            cv::Mat dct_img;
            cv::dct(float_img, dct_img);
            float_img.release();

            // Extract top-left 8x8 coefficients
            std::vector<float> dct_values;
            dct_values.reserve(64);

            for (int y = 0; y < 8; ++y)
            {
                for (int x = 0; x < 8; ++x)
                {
                    dct_values.push_back(dct_img.at<float>(y, x));
                }
            }

            dct_img.release();

            // Calculate median of ALL 64 DCT values
            std::vector<float> sorted_values = dct_values;
            std::sort(sorted_values.begin(), sorted_values.end());
            float median = sorted_values[sorted_values.size() / 2];

            // Generate 64-bit hash: 1 if > median, 0 otherwise
            std::uint64_t hash_value = 0;
            for (size_t i = 0; i < 64; ++i)
            {
                if (dct_values[i] > median)
                {
                    hash_value |= (1ULL << i);
                }
            }

            // Convert to byte array (8 bytes = 64 bits)
            out.phash64.resize(8);
            for (int i = 0; i < 8; ++i)
            {
                out.phash64[i] = static_cast<std::uint8_t>((hash_value >> (i * 8)) & 0xFF);
            }
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

            // Release original decoded image memory immediately (can be 70MB+ for large images)
            img.release();

            // Compute proper DCT-based perceptual hash (64-bit)
            cv::Mat gray;
            cv::cvtColor(resized, gray, cv::COLOR_BGR2GRAY);

            // Resize to 32x32 for DCT
            cv::Mat small;
            cv::resize(gray, small, cv::Size(32, 32), 0, 0, cv::INTER_AREA);

            // Release previous matrices
            resized.release();
            gray.release();

            // Convert to float for DCT
            cv::Mat float_img;
            small.convertTo(float_img, CV_32F);
            small.release();

            // Apply DCT
            cv::Mat dct_img;
            cv::dct(float_img, dct_img);
            float_img.release();

            // Extract top-left 8x8 coefficients
            std::vector<float> dct_values;
            dct_values.reserve(64);

            for (int y = 0; y < 8; ++y)
            {
                for (int x = 0; x < 8; ++x)
                {
                    dct_values.push_back(dct_img.at<float>(y, x));
                }
            }

            dct_img.release();

            // Calculate median of ALL 64 DCT values
            std::vector<float> sorted_values = dct_values;
            std::sort(sorted_values.begin(), sorted_values.end());
            float median = sorted_values[sorted_values.size() / 2];

            // Generate 64-bit hash: 1 if > median, 0 otherwise
            std::uint64_t hash_value = 0;
            for (size_t i = 0; i < 64; ++i)
            {
                if (dct_values[i] > median)
                {
                    hash_value |= (1ULL << i);
                }
            }

            // Convert to byte array
            out.phash64.resize(8);
            for (int i = 0; i < 8; ++i)
            {
                out.phash64[i] = static_cast<std::uint8_t>((hash_value >> (i * 8)) & 0xFF);
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
