#pragma once

#include <optional>
#include <string>
#include <unordered_map>

namespace MediaDedup
{
    /**
     * @brief EXIF-3: location name -> GPS bounding box, for prompt search.
     *
     * Loads a GeoJSON FeatureCollection (geodata/world_locations.geojson — a
     * compact gazetteer distilled from Natural Earth: countries, states,
     * US/CA parks, major cities) and answers case-insensitive name lookups
     * with the feature's bounding box. The search engine turns a matched
     * name in a query ("yellowstone") into a lat/lon BETWEEN filter on the
     * exif_gps_lat/exif_gps_lon columns.
     *
     * Features may carry an explicit `bbox` [west, south, east, north]
     * (the bundled gazetteer always does, with null geometry) or a Polygon /
     * MultiPolygon / Point geometry from which the box is computed — so a
     * user can drop in any Natural Earth GeoJSON instead.
     *
     * Thread safety: loadFromFile is NOT thread-safe; call it once before
     * concurrent lookup() calls (lookup is const and safe to share).
     */
    class LocationGeocoder
    {
    public:
        struct Bounds
        {
            double lat_min = 0.0;
            double lat_max = 0.0;
            double lon_min = 0.0;
            double lon_max = 0.0;
        };

        /// Parse a GeoJSON FeatureCollection. Returns false on unreadable
        /// file or malformed JSON; a readable file with zero named features
        /// loads "successfully" but leaves the geocoder empty.
        bool loadFromFile(const std::string &geojson_path);

        /**
         * @brief Case-insensitive lookup.
         *
         * Exact name match wins; otherwise a stored name that CONTAINS the
         * query matches ("yellowstone" -> "Yellowstone NP"), preferring the
         * smallest-area candidate (most specific place). Queries shorter
         * than 3 characters only match exactly, so "in"/"us" don't hit
         * substrings of everything.
         */
        std::optional<Bounds> lookup(const std::string &location_name) const;

        std::size_t size() const { return by_name_.size(); }

    private:
        // lowercase name -> bounds
        std::unordered_map<std::string, Bounds> by_name_;
    };
}
