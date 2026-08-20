#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "search/location_geocoder.hpp"

namespace MediaDedup
{
    /**
     * @brief EXIF-4: constraints parsed out of a natural-language query.
     *
     * "sunset photos taken with a canon iso above 1600 in yellowstone"
     * splits into structured filters (camera ~ canon, iso > 1600, GPS in
     * Yellowstone's bounds) plus the remaining semantic text ("sunset
     * photos") for the CLIP tower.
     */
    struct ExifQueryConstraints
    {
        struct Numeric
        {
            std::string column; // e.g. "exif_iso"
            std::string op;     // ">", "<", ">=", "<=", "=", "between"
            double value = 0.0;
            double value2 = 0.0; // upper bound when op == "between"
        };

        std::vector<Numeric> numeric;

        // Each term filters exif_camera LIKE '%term%' (make+model string).
        std::vector<std::string> camera_terms;

        // EXIF Flash tag bit 0 = "flash fired".
        bool flash_on = false;
        bool flash_off = false;

        std::optional<LocationGeocoder::Bounds> location;
        std::string location_name; // for logging only

        // Inclusive range on exif_datetime ("YYYY:MM:DD HH:MM:SS" strings —
        // lexicographic comparison works in that format).
        std::optional<std::pair<std::string, std::string>> date_range;

        // Month with no year: match substr(exif_datetime, 6, 2) == "08".
        std::optional<std::string> month_any_year;

        // What's left for semantic search after the EXIF tokens are removed.
        std::string remaining;

        // True when remaining is only filler ("photos", "all", "show me") —
        // the query is a pure metadata filter and CLIP should be skipped.
        bool remaining_is_filler = false;

        bool hasAny() const
        {
            return !numeric.empty() || !camera_terms.empty() || flash_on || flash_off ||
                   location.has_value() || date_range.has_value() || month_any_year.has_value();
        }
    };

    /**
     * @brief Parses EXIF constraints from free-text search queries.
     *
     * Recognized patterns (case-insensitive):
     * - Camera makes: "canon", "shot with a sony", "nikon camera", ...
     * - ISO: "iso 1600", "iso above/over 3200", "iso below/under 800",
     *   "iso between 400 and 1600"
     * - Aperture: "f/2.8" (approximate), "wider than f/2.8" (< 2.8),
     *   "narrower than f/5.6" (> 5.6)
     * - Shutter: "shutter 1/500" (approximate exposure time),
     *   "shutter faster than 1/500" (exposure_time < 1/500), "slower than"
     * - Focal length: "50mm" (approximate), "focal length above 200mm"
     * - Flash: "with flash" / "flash on"; "without flash" / "no flash"
     * - Dates: "2024" (year), "august 2024" (month), bare "august"
     *   (that month in any year)
     * - Locations, preposition-triggered: "in yellowstone", "at paris",
     *   "near utah", "from india" — resolved through the LocationGeocoder.
     *   Preposition-less place names are left to the CLIP text search
     *   (guessing turns city names like "Nice" or "Phoenix" into false GPS
     *   filters).
     *
     * The geocoder pointer may be null (location parsing is skipped).
     * parse() is const and thread-safe once the geocoder is loaded.
     */
    class ExifQueryParser
    {
    public:
        explicit ExifQueryParser(const LocationGeocoder *geocoder = nullptr)
            : geocoder_(geocoder) {}

        ExifQueryConstraints parse(const std::string &query) const;

    private:
        const LocationGeocoder *geocoder_;
    };
}
