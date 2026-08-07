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
        // QHD long-edge target for cached previews. Also the gate for using a
        // RAW file's embedded JPEG: embedded images at least this large serve
        // as the preview source instead of a full demosaic.
        static constexpr size_t MAX_PREVIEW_DIMENSION = 2560;

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
         * @brief Transcode a RAW image file to JPEG format
         *
         * @param file_path Path to the source RAW image file
         * @param jpeg_data Output vector to store transcoded JPEG data
         * @return true if transcoding succeeded, false otherwise
         */
        bool transcodeToJpeg(const std::string &file_path, std::vector<std::uint8_t> &jpeg_data,
                             int timeout_ms = 60000);

        /**
         * @brief Build the magick CLI argument vector for one QHD preview render.
         *
         * Exposed for unit testing. `-strip` drops the source's EXIF/XMP/ICC
         * profiles from the cached preview (they are re-encoded dead weight;
         * metadata is read from the original file, never from the preview).
         *
         * @param read_path Magick input spec (may carry a format prefix)
         * @param output_path Output JPEG path (written with a jpeg: prefix)
         */
        static std::vector<std::string> buildPreviewMagickArgs(const std::string &read_path,
                                                               const std::string &output_path);

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


    };
}
