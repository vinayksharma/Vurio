#pragma once

#include <optional>
#include <string>

namespace MediaDedup
{
    struct ExifInfo
    {
        // THUMB-1 contract: the image's BASE dimensions — the stored pixel
        // axes with the EXIF Orientation applied, i.e. the same axes the
        // rendered (-auto-orient) thumbnail has. This is the pre-edit
        // baseline, NOT the display geometry: once non-destructive edits
        // exist, display geometry = computeDisplayDimensions(base, chain)
        // (see docs/todo_aspect_ratio_thumbnails.md §3.1).
        int width = 0;
        int height = 0;
        std::string datetime; // EXIF style "YYYY:MM:DD HH:MM:SS"; empty if unknown
        std::string camera;   // "Make Model"; empty if unknown
    };

    /**
     * @brief MP-1: image metadata WITHOUT decoding pixels.
     *
     * The previous EXIF step ran `magick identify` on the ORIGINAL file —
     * for RAW files ImageMagick's delegate performs a FULL demosaic just to
     * report dimensions, one gratuitous RAW decode per ingested photo.
     *
     * - RAW: LibRaw open_file() parses the metadata blocks only (make,
     *   model, capture time, output dimensions) — no unpack, no demosaic,
     *   milliseconds instead of seconds, no subprocess.
     * - Everything else: `magick identify -ping` — ping mode reads headers
     *   and EXIF without decoding pixel data (the old call decoded the
     *   whole JPEG).
     */
    class ExifReader
    {
    public:
        static std::optional<ExifInfo> read(const std::string &file_path);

        /**
         * @brief THUMB-1: does an ImageMagick %[orientation] value swap the
         * width/height axes when auto-oriented? True for LeftTop, RightTop,
         * RightBottom, LeftBottom (EXIF 5-8). Exposed for unit tests.
         */
        static bool orientationTransposesAxes(const std::string &orientation);
    };
}
