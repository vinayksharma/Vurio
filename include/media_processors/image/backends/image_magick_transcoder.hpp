#pragma once

#include <Magick++.h>
#include <vector>
#include <string>
#include <memory>
#include <Poco/Logger.h>
#include <mutex>

namespace MediaDedup
{
    // Global initialization flag for ImageMagick (shared across all instances)
    extern std::once_flag magick_global_initialized;
    /**
     * @brief RAII-based ImageMagick transcoder with per-thread instances
     *
     * This class provides thread-safe transcoding by creating independent
     * ImageMagick instances per thread, eliminating the need for mutex locks.
     * Each thread gets its own transcoder instance, enabling parallel processing.
     */
    class ImageMagickTranscoder
    {
    public:
        /**
         * @brief Construct a new ImageMagick transcoder
         *
         * Each instance is independent and thread-safe. Multiple instances
         * can be created and used concurrently without synchronization.
         */
        ImageMagickTranscoder();

        /**
         * @brief Destroy the ImageMagick transcoder
         *
         * Automatically cleans up ImageMagick resources.
         */
        ~ImageMagickTranscoder();

        /**
         * @brief Transcode a RAW image file to TIFF format
         *
         * @param file_path Path to the source RAW image file
         * @param tiff_data Output vector to store transcoded TIFF data
         * @return true if transcoding succeeded, false otherwise
         */
        bool transcodeToTiff(const std::string &file_path, std::vector<std::uint8_t> &tiff_data);

        /**
         * @brief Check if this transcoder instance is valid
         *
         * @return true if the transcoder is ready for use, false otherwise
         */
        bool isValid() const { return valid_; }

        // Disable copy constructor and assignment operator
        ImageMagickTranscoder(const ImageMagickTranscoder &) = delete;
        ImageMagickTranscoder &operator=(const ImageMagickTranscoder &) = delete;

        // Allow move constructor but disable move assignment (logger_ is a reference)
        ImageMagickTranscoder(ImageMagickTranscoder &&) = default;
        ImageMagickTranscoder &operator=(ImageMagickTranscoder &&) = delete;

    private:
        Magick::Image image_;
        bool valid_;
        Poco::Logger &logger_;

        /**
         * @brief Initialize ImageMagick resources for this instance
         *
         * @return true if initialization succeeded, false otherwise
         */
        bool initialize();

        /**
         * @brief Configure image properties for optimal transcoding
         *
         * @param image Reference to the ImageMagick image to configure
         */
        void configureImageProperties(Magick::Image &image);

        /**
         * @brief Set TIFF format options for OpenCV compatibility
         *
         * @param image Reference to the ImageMagick image to configure
         */
        void setTiffFormatOptions(Magick::Image &image);
    };
}
