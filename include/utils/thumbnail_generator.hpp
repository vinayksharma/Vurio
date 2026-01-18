#pragma once

#include <string>
#include <cstdint>

namespace MediaDedup
{
    /**
     * @brief Utility for generating image thumbnails using ImageMagick
     *
     * Thread-safe static methods for creating JPEG thumbnails with configurable
     * size and quality. Supports ALL image formats including RAW files (ARW, CR2, NEF, etc.)
     * directly without transcoding. Runs synchronously in caller's thread.
     * Concurrency controlled by HTTP server thread pool.
     */
    class ThumbnailGenerator
    {
    public:
        /**
         * @brief Generate a thumbnail for an image file (synchronous)
         *
         * @param source_path Path to the source image file
         * @param output_path Path where the thumbnail JPEG will be saved
         * @param size Target size in pixels (longer edge will be this size)
         * @param quality JPEG quality (0-100, default 85)
         * @param timeout_ms Unused (kept for backward compatibility)
         * @return true if thumbnail generated successfully, false otherwise
         */
        static bool generate(const std::string &source_path,
                             const std::string &output_path,
                             int size,
                             int quality = 85,
                             int timeout_ms = 5000);

        /**
         * @brief Validate if a size is supported
         * @param size Size to validate
         * @return true if size is one of: 128, 256, 512, 1024
         */
        static bool isValidSize(int size);

        /**
         * @brief Get the closest valid size
         * @param requested_size Requested size
         * @return Closest valid size (128, 256, 512, or 1024)
         */
        static int getClosestValidSize(int requested_size);

    private:
        // Supported thumbnail sizes
        static constexpr int VALID_SIZES[] = {128, 256, 512, 1024};
        static constexpr int DEFAULT_SIZE = 256;
        static constexpr int DEFAULT_QUALITY = 85;
        static constexpr int DEFAULT_TIMEOUT_MS = 5000;
    };

} // namespace MediaDedup
