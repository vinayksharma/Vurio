#pragma once

#include <limits>
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

        // Location & Time (GPS). NaN = not present: (0,0) is a real ocean
        // fix, so a zero default would put every GPS-less photo there and
        // corrupt location filters. Stored as NULL when NaN (EXIF-5).
        float gps_lat = std::numeric_limits<float>::quiet_NaN();
        float gps_lon = std::numeric_limits<float>::quiet_NaN();
        float gps_alt = std::numeric_limits<float>::quiet_NaN();
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

        // RAW-EXIF conversion helpers for the LibRaw metadata path. Pure
        // functions, exposed for unit tests: no LibRaw-openable fixture can
        // be synthesized in-tree (LibRaw rejects minimal hand-built DNGs),
        // so the conversions are tested directly and the extraction itself
        // is verified against live RAW files.

        /// "1/500" for sub-second exposures, "2.5s"/"30s" otherwise; empty
        /// when seconds is not a positive finite value.
        static std::string shutterTextFromSeconds(float seconds);

        /// DMS triplet + hemisphere ref ('N'/'S'/'E'/'W', case-insensitive)
        /// to signed decimal degrees; NaN when the ref is not a valid
        /// hemisphere letter (LibRaw leaves it 0 when no GPS IFD exists,
        /// and (0,0) is a real ocean fix — see the ExifInfo GPS contract).
        static float gpsDecimalFromDms(float degrees, float minutes,
                                       float seconds, char hemisphere);

        /// LibRaw imgdata.sizes.flip to the EXIF Orientation code the same
        /// rendering implies: 0->1, 3->3 (180°), 5->8 (90° CCW), 6->6
        /// (90° CW); anything else -> 1.
        static uint8_t orientationFromLibRawFlip(int flip);
    };
}
