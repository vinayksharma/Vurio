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
        /// Visible (cropped) dimensions of the RAW image the preview was taken
        /// from, as LibRaw reports them; 0 when unavailable. Lets a caller tell
        /// a full-sensor embedded preview from a small one without reopening
        /// the file — the difference between "this IS the full-resolution
        /// image" and "a demosaic would give a much bigger one".
        int source_width = 0;
        int source_height = 0;

        int longEdge() const { return width > height ? width : height; }
        int sourceLongEdge() const { return source_width > source_height ? source_width : source_height; }

        /// True when the embedded preview carries essentially the RAW's full
        /// resolution, so demosaicing would buy nothing worth its cost. The
        /// 90% tolerance covers bodies whose preview is rendered from a
        /// slightly tighter crop than the reported visible area. Unknown
        /// source dimensions count as full size: that is the old behaviour,
        /// and it keeps a LibRaw that cannot report sizes from forcing a slow
        /// demosaic on every RAW.
        bool isFullSize() const
        {
            return sourceLongEdge() <= 0 || longEdge() * 10 >= sourceLongEdge() * 9;
        }
    };

    /**
     * @brief THE single RAW decode entry point for thumbnails and previews.
     *
     * Extracts the largest embedded thumbnail/preview from a RAW file (RAF,
     * ARW, NEF, ...) via LibRaw into a temp JPEG — largest as chosen from the
     * file's whole thumbnail list, not whichever one it nominates as default.
     * JPEG-type thumbs are written verbatim; bitmap-type thumbs (some camera
     * bodies) are encoded to JPEG in-process, so neither forces the slow
     * full-demosaic fallback anymore.
     *
     * Note that "largest embedded" is routinely well below the sensor's own
     * resolution (1616x1080 out of 5496x3672 is a normal Sony ARW): consult
     * RawEmbeddedJpeg::isFullSize() before treating the result as the RAW's
     * full-resolution image.
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
