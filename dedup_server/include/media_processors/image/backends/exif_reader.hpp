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

        // Geometry & Orientation
        std::string datetime;        // EXIF style "YYYY:MM:DD HH:MM:SS"; empty if unknown
        std::string camera;          // "Make Model"; empty if unknown
        uint8_t orientation = 1;     // EXIF Orientation (1-8, default 1)

        // Camera & Lens
        std::string make;            // Camera manufacturer
        std::string model;           // Camera model
        std::string lens_model;      // Lens name/model

        // Exposure & Light (Numeric)
        uint32_t iso = 0;            // ISO sensitivity
        float f_number = 0.0f;       // Aperture f-stop
        float shutter_speed_apex = 0.0f;  // APEX value for shutter speed
        std::string shutter_speed_text;   // Text form "1/1000" or "2"
        float exposure_time = 0.0f;  // Exposure time in seconds
        float exposure_comp = 0.0f;  // Exposure compensation in EV
        std::string metering_mode;   // "matrix", "center", "spot", "partial", etc.
        std::string flash_fired;     // "on", "off", "red-eye", etc.
        std::string flash_mode;      // Flash mode text

        // Optics
        float focal_length = 0.0f;   // Focal length in mm
        float focal_length_35mm = 0.0f;  // 35mm equivalent focal length
        float focus_distance = 0.0f; // Focus distance in meters
        std::string focus_mode;      // "auto", "manual", "continuous"

        // White Balance & Color
        std::string white_balance;   // "daylight", "tungsten", "fluorescent", "flash", "custom", "auto"
        std::string color_space;     // "sRGB", "Adobe RGB", etc.
        std::string scene_mode;      // "portrait", "landscape", "sports", "night", etc.

        // Light & Quality
        float brightness = 0.0f;     // Brightness value in EV
        std::string contrast;        // "normal", "low", "high"
        std::string saturation;      // "normal", "low", "high"
        std::string sharpness;       // "normal", "soft", "hard"

        // Location & Time (GPS)
        float gps_lat = 0.0f;        // Latitude (-90 to 90)
        float gps_lon = 0.0f;        // Longitude (-180 to 180)
        float gps_alt = 0.0f;        // Altitude in meters
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
