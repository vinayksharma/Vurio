#pragma once

#include <optional>
#include <string>

namespace MediaDedup
{
    /**
     * @brief The RAW file's embedded preview, materialized as a temp JPEG.
     *
     * The caller owns the temp file and must remove it when done.
     */
    struct RawEmbeddedJpeg
    {
        std::string jpeg_path;
        int width = 0;
        int height = 0;

        int longEdge() const { return width > height ? width : height; }
    };

    /**
     * @brief THE single RAW decode entry point for thumbnails and previews.
     *
     * Extracts the largest embedded thumbnail/preview from a RAW file (RAF,
     * ARW, NEF, ...) via LibRaw into a temp JPEG. JPEG-type thumbs are written
     * verbatim; bitmap-type thumbs (some camera bodies) are encoded to JPEG
     * in-process, so neither forces the slow full-demosaic fallback anymore.
     *
     * Returns nullopt when the file has no usable embedded image or LibRaw
     * cannot open it — callers then fall back to the external `magick`
     * full-RAW decode. Both ThumbnailGenerator and ImageMagickTranscoder MUST
     * route RAW input through this method; do not reintroduce per-call-site
     * LibRaw logic (that divergence caused RAFs with a thumbnail but no
     * preview, and vice versa).
     */
    class RawEmbeddedExtractor
    {
    public:
        static std::optional<RawEmbeddedJpeg> extract(const std::string &source_path);
    };

} // namespace MediaDedup
