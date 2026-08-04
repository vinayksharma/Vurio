#pragma once

#include <optional>
#include <string>

namespace MediaDedup
{
    struct ExifInfo
    {
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
    };
}
