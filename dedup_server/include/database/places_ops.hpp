#pragma once

#include <optional>
#include <string>
#include <vector>

namespace Poco { namespace Data { class Session; } }

namespace MediaDedup
{
    class DatabaseManager;

    /**
     * @brief PLACE-1 schema and row operations (docs/todo_place_search.md §4).
     *
     * places        — one row per named place a photo has been resolved to,
     *                 whatever the source (offline GeoNames gazetteer today;
     *                 online providers later). Only REFERENCED places live
     *                 here — the 9M-row gazetteer stays in its own read-only
     *                 file — so a name lookup at search time only ever sees
     *                 places the library actually has photos in, which is
     *                 what resolves "paris" to whichever Paris the user
     *                 visited.
     * file_places   — which places a file is "in"/"near", one row per role
     *                 (country, region, locality, park, landmark, ...).
     * scanned_files — gains place_label (display string, most specific
     *                 first) and place_level (0 = not resolved, 1 = offline
     *                 gazetteer, 2 = online provider).
     */
    class PlacesOps
    {
    public:
        static constexpr int kLevelNone = 0;
        // The offline level is the resolver VERSION: bumping it makes the
        // sweep revisit every row written by an older resolver (PLACE-4
        // bumped 1 -> 2 to fill places.fcode and shore_distance_m).
        static constexpr int kLevelOffline = 2;
        static constexpr int kLevelOnline = 10;

        struct PlaceRow
        {
            int id = 0;
            std::string source;      // geonames | naturalearth | apple | osm
            std::string external_id; // geonameid, feature name, provider id
            std::string name;
            std::string kind;        // country|region|county|district|locality|park|area|water|terrain|landmark|trail
            std::string fcode;       // PLACE-4: exact GeoNames code ("H.BAY"); empty for non-GeoNames rows
            double lat = 0.0;
            double lon = 0.0;
            std::string cc;          // ISO-3166 alpha-2, may be empty
        };

        struct FilePlace
        {
            PlaceRow place;
            std::string role;        // country|region|locality|park|landmark
            double distance_m = 0.0; // 0 for containment
        };

        struct FilePlaceRow
        {
            int place_id = 0;
            std::string role;
            double distance_m = 0.0;
            std::string source;
            std::string name;
            std::string kind;
        };

        struct Unresolved
        {
            int file_id = 0;
            double lat = 0.0;
            double lon = 0.0;
        };

        static bool ensureTable(DatabaseManager &db);

        /// Insert-or-find by (source, external_id); returns places.id, 0 on failure.
        static int upsertPlace(DatabaseManager &db, const PlaceRow &place);

        /**
         * @brief Delete the file_places rows of `file_ids` inside the
         * caller's open transaction (the FacesOps::deleteFacesForFiles
         * pattern, used by every scanned_files removal path). A row that
         * outlives its photo would keep its place "with photos" for
         * placeIdsByName. Safe on a database that never saw ensureTable.
         * Returns rows deleted.
         */
        static int deleteFilePlacesForFiles(Poco::Data::Session &sess,
                                            const std::vector<int> &file_ids);

        /// Places no file references any more (integrity-check cleanup).
        static int deleteOrphanPlaces(DatabaseManager &db);

        /**
         * @brief Replace a file's place rows, label and level in one
         * transaction. `places` may be empty (a GPS fix with nothing named
         * nearby) — the level still advances so the sweep converges.
         */
        static bool setFilePlaces(DatabaseManager &db, int file_id,
                                  const std::vector<FilePlace> &places,
                                  const std::string &label, int level,
                                  std::optional<double> shore_distance_m = std::nullopt);

        /**
         * @brief Ids of places WITH PHOTOS whose name matches `name` as a
         * whole word sequence: exact ("bozeman"), or contained on word
         * boundaries ("yellowstone" -> "Yellowstone National Park",
         * "West Yellowstone"). Case-insensitive. Empty when nothing has
         * photos there — the caller then falls back to bbox/CLIP.
         */
        static std::vector<int> placeIdsByName(DatabaseManager &db, const std::string &name);

        /// Files with a GPS fix whose place_level is below `min_level`.
        static std::vector<Unresolved> listUnresolved(DatabaseManager &db, int min_level, int limit);
        static int countUnresolved(DatabaseManager &db, int min_level);

        static std::vector<FilePlaceRow> filePlaces(DatabaseManager &db, int file_id);

        /// Reset every file's places (level back to 0) — a resolver-version
        /// bump or the user's "re-resolve" action.
        static int resetAll(DatabaseManager &db);
    };
}
