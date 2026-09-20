#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace MediaDedup
{
    class LocationGeocoder;

    /**
     * @brief PLACE-1: GPS fix -> named places, entirely offline.
     *
     * Reads the gazetteer `geodata/places.sqlite` built by
     * scripts/build_places_db.py (GeoNames, ~9M named features clustered by
     * 0.01° cell) and answers "what is this photo in / near":
     *
     *  - locality: the best populated place within `locality_max_km`,
     *    scored by distance discounted by population (a 100k city 3 km
     *    away beats a nameless hamlet 1 km away; a hamlet 200 m away
     *    wins). Its admin1 code and country code give region and country.
     *  - park: containment in a bounding box of the small Natural Earth
     *    gazetteer (LocationGeocoder, kind "park") — GeoNames parks are
     *    points, so "inside Yellowstone" needs a box, not a distance.
     *  - landmarks: named natural/man-made features within
     *    `landmark_radius_m` (waterfalls, peaks, lakes, monuments,
     *    campgrounds, trails...), nearest first, capped.
     *
     * Lookups are cell range scans on the gazetteer's clustered primary key
     * (no in-memory spatial index: the 9M-point file stays on disk, SQLite's
     * page cache does the rest). One connection, serialized by a mutex —
     * lookups are sub-millisecond, and the scan pipeline calls this once
     * per GPS-tagged photo.
     */
    class OfflineReverseGeocoder
    {
    public:
        struct Options
        {
            double locality_max_km = 30.0;
            double landmark_radius_m = 1500.0;
            int max_landmarks = 5;
            // A locality further than max(in_town_m, 16*sqrt(population))
            // metres away is "near", not "in".
            double in_town_m = 1500.0;
            // A landmark closer than this leads the label ("Fairy Falls · ...").
            double label_landmark_m = 500.0;
            // PLACE-4: shoreline distance is stored only up to this far.
            double shore_max_m = 5000.0;
        };

        struct Place
        {
            std::int64_t geonameid = 0;
            std::string name;
            std::string kind;   // locality|park|water|terrain|landmark|trail|area|region|country|...
            std::string fcode;  // "P.PPL", "H.OVF", ...
            double lat = 0.0;
            double lon = 0.0;
            std::string cc;
            std::string admin1; // GeoNames admin1 code within cc ("WY")
            int population = 0;
            double distance_m = 0.0;
            std::string role;   // country|region|locality|park|landmark
            std::string source; // geonames|naturalearth
            std::string external_id;
        };

        struct Resolution
        {
            std::vector<Place> places; // every role row to store
            std::string label;         // "Fairy Falls · Yellowstone NP · Wyoming"
            bool locality_is_near = false;
            // PLACE-4: metres to the nearest Natural Earth shoreline
            // (coast, lake shore, major river); nullopt beyond shore_max_m
            // or when the gazetteer has no shore table.
            std::optional<double> shore_distance_m;
        };

        OfflineReverseGeocoder();
        ~OfflineReverseGeocoder();
        OfflineReverseGeocoder(const OfflineReverseGeocoder &) = delete;
        OfflineReverseGeocoder &operator=(const OfflineReverseGeocoder &) = delete;

        /// Open the gazetteer read-only. False (and !isOpen()) when the
        /// file is missing/unreadable — place resolution is then skipped
        /// everywhere and the rest of the pipeline is unaffected.
        bool open(const std::string &places_sqlite_path, const Options &options);
        bool open(const std::string &places_sqlite_path);
        bool isOpen() const { return db_ != nullptr; }
        void close();

        /// Optional bbox gazetteer for park containment (kind "park").
        /// Shared: the resolver's sweep thread may still be inside resolve()
        /// when the owner of the gazetteer goes away, so the geocoder keeps
        /// it alive itself.
        void setBoundsGazetteer(std::shared_ptr<const LocationGeocoder> gazetteer)
        {
            bounds_owned_ = std::move(gazetteer);
            bounds_ = bounds_owned_.get();
        }
        /// Non-owning form (tests with a stack gazetteer that outlives this).
        void setBoundsGazetteer(const LocationGeocoder *gazetteer)
        {
            bounds_owned_.reset();
            bounds_ = gazetteer;
        }

        /// Resolve a fix. nullopt only when the geocoder is not open; an
        /// open geocoder always answers (possibly with an empty Resolution
        /// for a mid-ocean fix).
        std::optional<Resolution> resolve(double lat, double lon) const;

        /// PLACE-4: metres to the nearest shoreline segment within
        /// options().shore_max_m; nullopt when none or no shore table.
        std::optional<double> shoreDistance(double lat, double lon) const;
        bool hasShore() const { return shore_stmt_ != nullptr; }

        /// Human-readable name for a GeoNames admin1 code / country code.
        std::string regionName(const std::string &cc, const std::string &admin1) const;
        std::string countryName(const std::string &cc) const;

        const Options &options() const { return options_; }
        std::size_t size() const { return row_count_; }

        static double haversineMeters(double lat1, double lon1, double lat2, double lon2);

    private:
        struct FcodeInfo { std::string code; std::string kind; };
        struct AdminInfo { std::string name; std::int64_t geonameid; };

        // All gazetteer rows in the cell square [lat_idx±r_lat] x [lon_idx±r_lon].
        void fetchSquare(int lat_idx, int lon_idx, int r_lat, int r_lon,
                         double lat, double lon, std::vector<Place> &out) const;

        sqlite3 *db_ = nullptr;
        sqlite3_stmt *cell_stmt_ = nullptr;
        sqlite3_stmt *shore_stmt_ = nullptr;
        mutable std::mutex mutex_;
        Options options_;
        std::size_t row_count_ = 0;
        std::unordered_map<int, FcodeInfo> fcodes_;
        std::unordered_map<std::string, AdminInfo> countries_; // cc -> name
        std::unordered_map<std::string, AdminInfo> admin1_;    // "US.WY" -> name
        std::shared_ptr<const LocationGeocoder> bounds_owned_;
        const LocationGeocoder *bounds_ = nullptr;
    };
}
