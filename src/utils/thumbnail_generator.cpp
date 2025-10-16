#include "utils/thumbnail_generator.hpp"
#include <Poco/Logger.h>
#include <filesystem>
#include <chrono>
#include <thread>
#include <future>
#include <algorithm>

#ifdef HAVE_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace MediaDedup
{
    bool ThumbnailGenerator::generate(const std::string &source_path,
                                      const std::string &output_path,
                                      int size,
                                      int quality,
                                      int timeout_ms)
    {
#ifdef HAVE_OPENCV
        try
        {
            Poco::Logger &logger = Poco::Logger::get("ThumbnailGenerator");

            // Validate size
            if (!isValidSize(size))
            {
                logger.warning("Invalid thumbnail size %d requested, using closest valid size", size);
                size = getClosestValidSize(size);
            }

            // Check if source file exists
            if (!std::filesystem::exists(source_path))
            {
                logger.error("Source file not found: %s", source_path);
                return false;
            }

            // Create a promise/future for timeout handling
            std::promise<bool> promise;
            std::future<bool> future = promise.get_future();

            std::thread worker([&promise, source_path, output_path, size, quality, &logger]()
                               {
                try
                {
                    // Disable OpenCV internal threading to let TPM control concurrency
                    cv::setNumThreads(0);

                    // Load image
                    cv::Mat img = cv::imread(source_path, cv::IMREAD_COLOR);
                    if (img.empty())
                    {
                        logger.warning("Failed to load image: %s", source_path);
                        promise.set_value(false);
                        return;
                    }

                    int orig_w = img.cols;
                    int orig_h = img.rows;

                    if (orig_w == 0 || orig_h == 0)
                    {
                        logger.warning("Invalid image dimensions: %dx%d for %s", orig_w, orig_h, source_path);
                        promise.set_value(false);
                        return;
                    }

                    // Calculate new dimensions maintaining aspect ratio
                    int new_w, new_h;
                    if (orig_w > orig_h)
                    {
                        new_w = size;
                        new_h = static_cast<int>((static_cast<double>(orig_h) / orig_w) * size);
                    }
                    else
                    {
                        new_h = size;
                        new_w = static_cast<int>((static_cast<double>(orig_w) / orig_h) * size);
                    }

                    // Ensure minimum dimensions
                    if (new_w <= 0) new_w = 1;
                    if (new_h <= 0) new_h = 1;

                    // Resize image
                    cv::Mat thumbnail;
                    cv::resize(img, thumbnail, cv::Size(new_w, new_h), 0, 0, cv::INTER_AREA);

                    // Release original image memory
                    img.release();

                    // Save as JPEG
                    std::vector<int> jpeg_params = {cv::IMWRITE_JPEG_QUALITY, quality};
                    bool success = cv::imwrite(output_path, thumbnail, jpeg_params);

                    // Release thumbnail memory
                    thumbnail.release();

                    if (success)
                    {
                        logger.debug("Generated thumbnail: %s (%dx%d) -> %s (%dx%d)",
                                   source_path, orig_w, orig_h, output_path, new_w, new_h);
                    }
                    else
                    {
                        logger.error("Failed to save thumbnail: %s", output_path);
                    }

                    promise.set_value(success);
                }
                catch (const std::exception &e)
                {
                    logger.error("Exception in thumbnail generation: %s", std::string(e.what()));
                    promise.set_value(false);
                }
                catch (...)
                {
                    logger.error("Unknown exception in thumbnail generation");
                    promise.set_value(false);
                } });

            // Wait for completion with timeout
            auto status = future.wait_for(std::chrono::milliseconds(timeout_ms));

            if (status == std::future_status::timeout)
            {
                logger.warning("Thumbnail generation timed out after %d ms: %s", timeout_ms, source_path);
                // Worker thread will be abandoned - it will complete eventually
                worker.detach();
                return false;
            }

            worker.join();
            return future.get();
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ThumbnailGenerator").error("Exception in generate: %s", std::string(e.what()));
            return false;
        }
#else
        Poco::Logger::get("ThumbnailGenerator").error("OpenCV not available - cannot generate thumbnails");
        return false;
#endif
    }

    bool ThumbnailGenerator::isValidSize(int size)
    {
        for (int valid_size : VALID_SIZES)
        {
            if (size == valid_size)
                return true;
        }
        return false;
    }

    int ThumbnailGenerator::getClosestValidSize(int requested_size)
    {
        if (requested_size <= 0)
            return DEFAULT_SIZE;

        int closest = VALID_SIZES[0];
        int min_diff = std::abs(requested_size - closest);

        for (int valid_size : VALID_SIZES)
        {
            int diff = std::abs(requested_size - valid_size);
            if (diff < min_diff)
            {
                min_diff = diff;
                closest = valid_size;
            }
        }

        return closest;
    }

} // namespace MediaDedup
