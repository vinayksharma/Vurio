#pragma once

#include <string>
#include <cstdint>
#include <vector>
#include <Magick++.h>

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
         * @brief Generate a thumbnail to a blob in memory
         *
         * @param source_path Path to the source image file
         * @param size Target size in pixels (longer edge will be this size)
         * @param quality JPEG quality (0-100, default 85)
         * @param blob Output blob containing the JPEG thumbnail data
         * @param timeout_ms Hard limit on the magick subprocess; expired decodes are killed
         * @return true if thumbnail generated successfully, false otherwise
         */
        static bool generateToBlob(const std::string &source_path,
                                    int size,
                                    Magick::Blob &blob,
                                    int quality = 85,
                                    int timeout_ms = DEFAULT_TIMEOUT_MS);


        /**
         * @brief Build the magick CLI argument vector for one thumbnail render.
         *
         * Exposed for unit testing. `-strip` is load-bearing: without it the
         * source's EXIF/XMP/ICC profiles are copied into every thumbnail and
         * dwarf the pixel data at thumbnail sizes (measured 75-98% of output
         * bytes); `-colorspace sRGB` already fixes the rendering intent.
         *
         * @param read_path Magick input spec (may carry a format prefix)
         * @param output_path Output JPEG path (written with a jpeg: prefix)
         * @param size Longest-edge target; <= 0 skips the resize (native)
         * @param quality JPEG quality
         */
        static std::vector<std::string> buildMagickArgs(const std::string &read_path,
                                                        const std::string &output_path,
                                                        int size,
                                                        int quality);

        /**
         * @brief MP-3: explicit native-resolution sentinel.
         *
         * Renders at source resolution and is ONLY for the full-res viewer
         * paths, which stream/hold the result themselves and never store it
         * in the thumbnail cache. It is deliberately NOT a valid cached
         * thumbnail size: size=0 used to pass validation and produce
         * full-resolution "thumbnails" cached forever that the best-fit
         * query could never serve.
         */
        static constexpr int kNativeResolution = 0;

        /**
         * @brief Validate if a size is supported for CACHED thumbnails
         * @param size Size to validate
         * @return true if size is one of: 128, 256, 512, 1024
         */
        static bool isValidSize(int size);

        /**
         * @brief Get the closest valid cached-thumbnail size
         * @param requested_size Requested size
         * @return Closest valid size (128, 256, 512, or 1024)
         */
        static int getClosestValidSize(int requested_size);

    private:
        // Supported CACHED thumbnail sizes (native resolution is the
        // kNativeResolution sentinel, never cached)
        static constexpr int VALID_SIZES[] = {128, 256, 512, 1024};
        static constexpr int DEFAULT_SIZE = 256;
        static constexpr int DEFAULT_QUALITY = 85;

    public:
        // Hard cap on one magick subprocess. Generous enough for a legitimate
        // full-RAW demosaic on a slow volume; a hung decoder is killed so it
        // can never pin a pool thread forever.
        static constexpr int DEFAULT_TIMEOUT_MS = 60000;

    private:
    };

} // namespace MediaDedup
