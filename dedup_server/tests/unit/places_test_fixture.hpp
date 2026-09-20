#pragma once

// PLACE-1 test fixture: a miniature offline gazetteer in the exact schema
// scripts/build_places_db.py emits, so OfflineReverseGeocoder is tested
// against the real file layout (cells, 1e-5° integer coordinates, fcode
// ids, admin1/countries tables) without the 730 MB production file.

#include <sqlite3.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace MediaDedup::Test
{
    struct GazetteerRow
    {
        std::int64_t id;
        const char *name;
        double lat, lon;
        int fcode;       // index into kFcodes
        const char *cc;
        const char *admin1;
        int population;
    };

    // id, code, kind — mirrors fcodes(id, code, kind, label).
    inline const std::vector<std::tuple<int, const char *, const char *>> kFcodes = {
        {1, "P.PPLA2", "locality"}, {2, "P.PPL", "locality"}, {3, "H.OVF", "water"},
        {4, "H.SPNG", "water"},     {5, "L.PRK", "park"},     {6, "T.MT", "terrain"},
        {7, "H.PND", "water"},
    };

    // Montana / Wyoming corner of the real gazetteer, coordinates real.
    inline const std::vector<GazetteerRow> kRows = {
        {5641727, "Bozeman", 45.67965, -111.03856, 1, "US", "MT", 53293},
        {5679571, "West Yellowstone", 44.66215, -111.10411, 2, "US", "MT", 1271},
        {9000001, "Tinyville", 45.60000, -111.30000, 2, "US", "MT", 0},
        {9000002, "Suburbia", 45.70000, -111.00000, 2, "US", "MT", 0},
        {5590264, "Fairy Falls", 44.52492, -110.87102, 3, "US", "WY", 0},
        {5592625, "Old Faithful Geyser", 44.46046, -110.82815, 4, "US", "WY", 0},
        {5844092, "Yellowstone National Park", 44.50000, -110.50000, 5, "US", "WY", 0},
        {5601538, "Lone Mountain", 45.27916, -111.45021, 6, "US", "MT", 0},
        // Hudson waterfront: a big city's point across the river is nearer
        // than the real town's, but the landmark right here is in NJ.
        {5128581, "New York City", 40.71427, -74.00597, 1, "US", "NY", 8804190},
        {5099836, "Jersey City", 40.72816, -74.07764, 1, "US", "NJ", 262075},
        {9000010, "Newport Waterfront Park", 40.72750, -74.03400, 5, "US", "NJ", 0},
        // Review fix (per-kind landmark coverage): two kinds near Bozeman.
        {9000011, "Bozeman Pond", 45.69000, -111.04000, 7, "US", "MT", 0},
        {9000012, "Peets Hill", 45.67900, -111.03300, 6, "US", "MT", 0},
    };

    inline std::int64_t cellOf(double lat, double lon)
    {
        int lat_idx = static_cast<int>(std::floor((lat + 90.0) * 100.0));
        int lon_idx = static_cast<int>(std::floor((lon + 180.0) * 100.0));
        lat_idx = std::min(std::max(lat_idx, 0), 17999);
        lon_idx = std::min(std::max(lon_idx, 0), 35999);
        return static_cast<std::int64_t>(lat_idx) * 36000 + lon_idx;
    }

    /// Writes the fixture gazetteer; returns its path ("" on failure).
    inline std::string writeMiniGazetteer(const std::string &name = "mini_places.sqlite")
    {
        const std::string path = (std::filesystem::temp_directory_path() / name).string();
        std::remove(path.c_str());
        sqlite3 *db = nullptr;
        if (sqlite3_open(path.c_str(), &db) != SQLITE_OK)
            return "";
        auto exec = [&](const std::string &sql) {
            char *err = nullptr;
            int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err);
            if (err)
                sqlite3_free(err);
            return rc == SQLITE_OK;
        };
        bool ok = exec(
            "CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT);"
            "CREATE TABLE fcodes(id INTEGER PRIMARY KEY, code TEXT UNIQUE, kind TEXT NOT NULL, label TEXT);"
            "CREATE TABLE countries(cc TEXT PRIMARY KEY, name TEXT NOT NULL, geonameid INTEGER);"
            "CREATE TABLE admin1(code TEXT PRIMARY KEY, name TEXT NOT NULL, geonameid INTEGER);"
            "CREATE TABLE places(cell INTEGER NOT NULL, id INTEGER NOT NULL, name TEXT NOT NULL, ascii TEXT,"
            " lat INTEGER NOT NULL, lon INTEGER NOT NULL, fcode INTEGER NOT NULL, cc TEXT, admin1 TEXT,"
            " population INTEGER, PRIMARY KEY (cell, id)) WITHOUT ROWID;"
            "INSERT INTO meta VALUES ('rows', '13'), ('schema_version', '1');"
            // PLACE-4: one lake-shore segment just south of the Bozeman fix
            // used by the tests (lat 45.68, lon -111.045..-111.040).
            "CREATE TABLE shore(cell INTEGER NOT NULL, kind TEXT NOT NULL, lat1 INTEGER NOT NULL,"
            " lon1 INTEGER NOT NULL, lat2 INTEGER NOT NULL, lon2 INTEGER NOT NULL);"
            "CREATE INDEX idx_shore_cell ON shore(cell);"
            "INSERT INTO shore VALUES ((1356 * 3600) + 689, 'lake', 4568000, -11104500, 4568000, -11104000);"
            "INSERT INTO countries VALUES ('US', 'United States', 6252001);"
            "INSERT INTO admin1 VALUES ('US.MT', 'Montana', 5667009), ('US.WY', 'Wyoming', 5843591), "
            "('US.NY', 'New York', 5128638), ('US.NJ', 'New Jersey', 5101760);");
        for (const auto &[id, code, kind] : kFcodes)
            ok = ok && exec("INSERT INTO fcodes VALUES (" + std::to_string(id) + ", '" + code + "', '" +
                            kind + "', NULL);");
        for (const auto &r : kRows)
        {
            ok = ok && exec("INSERT INTO places VALUES (" + std::to_string(cellOf(r.lat, r.lon)) + ", " +
                            std::to_string(r.id) + ", '" + r.name + "', NULL, " +
                            std::to_string(static_cast<std::int64_t>(std::llround(r.lat * 1e5))) + ", " +
                            std::to_string(static_cast<std::int64_t>(std::llround(r.lon * 1e5))) + ", " +
                            std::to_string(r.fcode) + ", '" + r.cc + "', '" + r.admin1 + "', " +
                            (r.population ? std::to_string(r.population) : "NULL") + ");");
        }
        sqlite3_close(db);
        return ok ? path : "";
    }

    /// The bbox gazetteer with Yellowstone as a park (LocationGeocoder form).
    inline std::string writeMiniBounds(const std::string &name = "mini_bounds.geojson")
    {
        const std::string path = (std::filesystem::temp_directory_path() / name).string();
        FILE *f = std::fopen(path.c_str(), "w");
        if (!f)
            return "";
        std::fputs(R"({"type": "FeatureCollection", "features": [
            {"type": "Feature", "properties": {"name": "Yellowstone NP", "kind": "park"},
             "bbox": [-111.1544, 44.1343, -109.8265, 45.1001], "geometry": null},
            {"type": "Feature", "properties": {"name": "Montana", "kind": "state"},
             "bbox": [-116.05, 44.36, -104.04, 49.0], "geometry": null}]})",
                   f);
        std::fclose(f);
        return path;
    }
}
