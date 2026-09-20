#include "search/offline_reverse_geocoder.hpp"

#include "search/location_geocoder.hpp"

#include <Poco/Logger.h>
#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace MediaDedup
{
    namespace
    {
        constexpr int kLonStride = 36000;
        constexpr double kPi = 3.14159265358979323846;

        int latIndex(double lat)
        {
            int i = static_cast<int>(std::floor((lat + 90.0) * 100.0));
            return std::min(std::max(i, 0), 17999);
        }
        int lonIndex(double lon)
        {
            int i = static_cast<int>(std::floor((lon + 180.0) * 100.0));
            return std::min(std::max(i, 0), kLonStride - 1);
        }

        std::string columnText(sqlite3_stmt *s, int col)
        {
            const unsigned char *t = sqlite3_column_text(s, col);
            return t ? reinterpret_cast<const char *>(t) : std::string();
        }

        bool isLandmarkKind(const std::string &kind)
        {
            return kind == "water" || kind == "terrain" || kind == "landmark" ||
                   kind == "park" || kind == "trail" || kind == "area";
        }
    }

    OfflineReverseGeocoder::OfflineReverseGeocoder() = default;

    OfflineReverseGeocoder::~OfflineReverseGeocoder()
    {
        close();
    }

    void OfflineReverseGeocoder::close()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cell_stmt_)
        {
            sqlite3_finalize(cell_stmt_);
            cell_stmt_ = nullptr;
        }
        if (shore_stmt_)
        {
            sqlite3_finalize(shore_stmt_);
            shore_stmt_ = nullptr;
        }
        if (db_)
        {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        fcodes_.clear();
        countries_.clear();
        admin1_.clear();
        row_count_ = 0;
    }

    bool OfflineReverseGeocoder::open(const std::string &path)
    {
        return open(path, Options());
    }

    bool OfflineReverseGeocoder::open(const std::string &path, const Options &options)
    {
        close();
        Poco::Logger &logger = Poco::Logger::get("OfflineReverseGeocoder");
        std::lock_guard<std::mutex> lock(mutex_);
        options_ = options;

        sqlite3 *db = nullptr;
        // Read-only, no mutex (we serialize ourselves), and never create.
        int rc = sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr);
        if (rc != SQLITE_OK || !db)
        {
            logger.warning("cannot open gazetteer %s: %s", path,
                           std::string(db ? sqlite3_errmsg(db) : "open failed"));
            if (db)
                sqlite3_close(db);
            return false;
        }

        auto loadTable = [&](const char *sql, auto &&each) -> bool {
            sqlite3_stmt *s = nullptr;
            if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK)
            {
                logger.warning("gazetteer %s lacks expected table (%s)", path,
                               std::string(sqlite3_errmsg(db)));
                return false;
            }
            while (sqlite3_step(s) == SQLITE_ROW)
                each(s);
            sqlite3_finalize(s);
            return true;
        };

        bool ok = loadTable("SELECT id, code, kind FROM fcodes", [&](sqlite3_stmt *s) {
            fcodes_[sqlite3_column_int(s, 0)] = {columnText(s, 1), columnText(s, 2)};
        });
        ok = ok && loadTable("SELECT cc, name, geonameid FROM countries", [&](sqlite3_stmt *s) {
            countries_[columnText(s, 0)] = {columnText(s, 1), sqlite3_column_int64(s, 2)};
        });
        ok = ok && loadTable("SELECT code, name, geonameid FROM admin1", [&](sqlite3_stmt *s) {
            admin1_[columnText(s, 0)] = {columnText(s, 1), sqlite3_column_int64(s, 2)};
        });
        ok = ok && loadTable("SELECT value FROM meta WHERE key = 'rows'", [&](sqlite3_stmt *s) {
            row_count_ = static_cast<std::size_t>(sqlite3_column_int64(s, 0));
        });
        if (!ok)
        {
            sqlite3_close(db);
            fcodes_.clear();
            countries_.clear();
            admin1_.clear();
            return false;
        }

        sqlite3_stmt *cell_stmt = nullptr;
        rc = sqlite3_prepare_v2(db,
                                "SELECT id, name, lat, lon, fcode, cc, admin1, population "
                                "FROM places WHERE cell BETWEEN ? AND ?",
                                -1, &cell_stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            logger.warning("gazetteer %s: cannot prepare cell query (%s)", path,
                           std::string(sqlite3_errmsg(db)));
            sqlite3_close(db);
            return false;
        }
        db_ = db;
        cell_stmt_ = cell_stmt;
        // PLACE-4: optional shore table (older gazetteers lack it).
        sqlite3_stmt *shore_stmt = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT lat1, lon1, lat2, lon2 FROM shore WHERE cell BETWEEN ? AND ?",
                               -1, &shore_stmt, nullptr) == SQLITE_OK)
            shore_stmt_ = shore_stmt;
        else
            logger.information("gazetteer %s has no shore table; shoreline distance disabled "
                               "(rebuild with scripts/build_places_db.py --update-shore)", path);
        logger.information("Opened offline gazetteer %s (%z places, %z feature codes%s)",
                           path, row_count_, fcodes_.size(),
                           std::string(shore_stmt_ ? ", shoreline" : ""));
        return true;
    }

    double OfflineReverseGeocoder::haversineMeters(double lat1, double lon1, double lat2, double lon2)
    {
        const double r = 6371000.0;
        const double p1 = lat1 * kPi / 180.0, p2 = lat2 * kPi / 180.0;
        const double dp = (lat2 - lat1) * kPi / 180.0;
        const double dl = (lon2 - lon1) * kPi / 180.0;
        const double a = std::sin(dp / 2) * std::sin(dp / 2) +
                         std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
        return 2.0 * r * std::asin(std::min(1.0, std::sqrt(a)));
    }

    std::optional<double> OfflineReverseGeocoder::shoreDistance(double lat, double lon) const
    {
        if (!db_ || !shore_stmt_)
            return std::nullopt;
        // 0.1° shore cells; ±1 cell (≈ ±11 km) covers shore_max_m.
        constexpr int kShoreLonStride = 3600;
        const int lat_idx = std::min(std::max(static_cast<int>(std::floor((lat + 90.0) * 10.0)), 0), 1799);
        const int lon_idx = std::min(std::max(static_cast<int>(std::floor((lon + 180.0) * 10.0)), 0),
                                     kShoreLonStride - 1);
        // Local equirectangular metres.
        const double m_per_deg_lat = 111320.0;
        const double m_per_deg_lon = 111320.0 * std::max(0.05, std::cos(lat * kPi / 180.0));
        double best = std::numeric_limits<double>::infinity();

        std::lock_guard<std::mutex> lock(mutex_);
        for (int row = std::max(0, lat_idx - 1); row <= std::min(1799, lat_idx + 1); ++row)
        {
            const std::int64_t first = static_cast<std::int64_t>(row) * kShoreLonStride +
                                       std::max(0, lon_idx - 1);
            const std::int64_t last = static_cast<std::int64_t>(row) * kShoreLonStride +
                                      std::min(kShoreLonStride - 1, lon_idx + 1);
            sqlite3_reset(shore_stmt_);
            sqlite3_bind_int64(shore_stmt_, 1, first);
            sqlite3_bind_int64(shore_stmt_, 2, last);
            while (sqlite3_step(shore_stmt_) == SQLITE_ROW)
            {
                const double x1 = (sqlite3_column_int64(shore_stmt_, 1) / 1e5 - lon) * m_per_deg_lon;
                const double y1 = (sqlite3_column_int64(shore_stmt_, 0) / 1e5 - lat) * m_per_deg_lat;
                const double x2 = (sqlite3_column_int64(shore_stmt_, 3) / 1e5 - lon) * m_per_deg_lon;
                const double y2 = (sqlite3_column_int64(shore_stmt_, 2) / 1e5 - lat) * m_per_deg_lat;
                // Point (origin) to segment distance.
                const double dx = x2 - x1, dy = y2 - y1;
                const double len2 = dx * dx + dy * dy;
                double t = len2 > 0.0 ? -(x1 * dx + y1 * dy) / len2 : 0.0;
                t = std::min(1.0, std::max(0.0, t));
                const double px = x1 + t * dx, py = y1 + t * dy;
                best = std::min(best, std::sqrt(px * px + py * py));
            }
        }
        if (!std::isfinite(best) || best > options_.shore_max_m)
            return std::nullopt;
        return best;
    }

    std::string OfflineReverseGeocoder::regionName(const std::string &cc, const std::string &admin1) const
    {
        if (cc.empty() || admin1.empty())
            return "";
        auto it = admin1_.find(cc + "." + admin1);
        return it == admin1_.end() ? "" : it->second.name;
    }

    std::string OfflineReverseGeocoder::countryName(const std::string &cc) const
    {
        auto it = countries_.find(cc);
        return it == countries_.end() ? "" : it->second.name;
    }

    void OfflineReverseGeocoder::fetchSquare(int lat_idx, int lon_idx, int r_lat, int r_lon,
                                             double lat, double lon, std::vector<Place> &out) const
    {
        // Caller holds mutex_.
        const int lat_lo = std::max(0, lat_idx - r_lat), lat_hi = std::min(17999, lat_idx + r_lat);
        const int lon_lo = std::max(0, lon_idx - r_lon), lon_hi = std::min(kLonStride - 1, lon_idx + r_lon);
        for (int row = lat_lo; row <= lat_hi; ++row)
        {
            const std::int64_t first = static_cast<std::int64_t>(row) * kLonStride + lon_lo;
            const std::int64_t last = static_cast<std::int64_t>(row) * kLonStride + lon_hi;
            sqlite3_reset(cell_stmt_);
            sqlite3_bind_int64(cell_stmt_, 1, first);
            sqlite3_bind_int64(cell_stmt_, 2, last);
            while (sqlite3_step(cell_stmt_) == SQLITE_ROW)
            {
                Place p;
                p.geonameid = sqlite3_column_int64(cell_stmt_, 0);
                p.name = columnText(cell_stmt_, 1);
                p.lat = sqlite3_column_int64(cell_stmt_, 2) / 1e5;
                p.lon = sqlite3_column_int64(cell_stmt_, 3) / 1e5;
                auto fc = fcodes_.find(sqlite3_column_int(cell_stmt_, 4));
                if (fc != fcodes_.end())
                {
                    p.fcode = fc->second.code;
                    p.kind = fc->second.kind;
                }
                p.cc = columnText(cell_stmt_, 5);
                p.admin1 = columnText(cell_stmt_, 6);
                p.population = sqlite3_column_int(cell_stmt_, 7);
                p.distance_m = haversineMeters(lat, lon, p.lat, p.lon);
                p.source = "geonames";
                p.external_id = std::to_string(p.geonameid);
                out.push_back(std::move(p));
            }
        }
    }

    std::optional<OfflineReverseGeocoder::Resolution>
    OfflineReverseGeocoder::resolve(double lat, double lon) const
    {
        if (!db_ || std::isnan(lat) || std::isnan(lon) ||
            lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0)
            return std::nullopt;

        Resolution res;
        const int lat_idx = latIndex(lat), lon_idx = lonIndex(lon);
        // Longitude cells narrow with latitude: widen the east-west radius
        // so the square stays roughly square in metres.
        const double cos_lat = std::max(0.2, std::cos(lat * kPi / 180.0));
        auto lonRadius = [&](int r_lat) {
            return std::min(kLonStride / 2, static_cast<int>(std::ceil(r_lat / cos_lat)));
        };

        std::vector<Place> rows;
        std::optional<Place> locality;
        // Best populated place among `rows`, optionally restricted to one
        // cc/admin1. Distance discounted by population: bigger places
        // "reach" further. 1 + 1.5·log10(1+pop): hamlet ×1, 1k ×5.5,
        // 100k ×8.5, 10M ×11.5 — Paris at 4 km beats its own
        // arrondissement at 500 m; a hamlet 300 m away still beats a city
        // 20 km off.
        auto pickLocality = [&](const std::vector<Place> &cands, const Place *same_admin_as)
            -> std::optional<Place> {
            std::optional<Place> best;
            double best_score = 0.0;
            for (const auto &p : cands)
            {
                if (p.kind != "locality" || p.distance_m > options_.locality_max_km * 1000.0)
                    continue;
                if (same_admin_as && (p.cc != same_admin_as->cc || p.admin1 != same_admin_as->admin1))
                    continue;
                const double score = p.distance_m / (1.0 + 1.5 * std::log10(1.0 + p.population));
                if (!best || score < best_score)
                {
                    best = p;
                    best_score = score;
                }
            }
            return best;
        };
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // The first square (≈±5.5 km) covers the landmark radius AND
            // the reach of a mid-size town, so the population discount
            // below compares a city a few km off against the hamlet next
            // door instead of stopping at whichever is inside 2 km. Widen
            // only while no locality has been found, up to the configured
            // maximum (0.01° ≈ 1.11 km of latitude per cell).
            const int max_cells = std::max(5, static_cast<int>(std::ceil(options_.locality_max_km / 1.11)));
            int prev_r = -1;
            for (int r : {5, 12, 30, max_cells})
            {
                r = std::min(r, max_cells);
                if (r <= prev_r)
                    break;
                prev_r = r;
                // Rows already fetched in the smaller square are re-read
                // (cells are cheap); dedupe by geonameid below.
                rows.clear();
                fetchSquare(lat_idx, lon_idx, r, lonRadius(r), lat, lon, rows);
                locality = pickLocality(rows, nullptr);
                if (locality)
                    break;
                if (r >= max_cells)
                    break;
            }
        }

        std::vector<Place> landmarks;
        std::set<std::int64_t> seen;
        for (const auto &p : rows)
        {
            if (!isLandmarkKind(p.kind) || p.distance_m > options_.landmark_radius_m)
                continue;
            // GNIS carries vanished features as "X (historical)"; a photo is
            // never "at" one of those.
            if (p.name.find("(historical)") != std::string::npos)
                continue;
            if (!seen.insert(p.geonameid).second)
                continue;
            landmarks.push_back(p);
        }
        std::sort(landmarks.begin(), landmarks.end(),
                  [](const Place &a, const Place &b) { return a.distance_m < b.distance_m; });
        if (static_cast<int>(landmarks.size()) > options_.max_landmarks)
            landmarks.resize(static_cast<std::size_t>(options_.max_landmarks));

        // A feature right here (≤ label radius) knows which state it is in;
        // the town must agree. Otherwise a big city's centre point across
        // a river or border outscores the real town: the Jersey City
        // waterfront is nearer Manhattan's point than Jersey City's, but
        // its landmarks are all in New Jersey.
        if (!landmarks.empty() && landmarks.front().distance_m <= options_.label_landmark_m &&
            !landmarks.front().cc.empty() && !landmarks.front().admin1.empty())
        {
            const Place &here = landmarks.front();
            if (!locality || locality->cc != here.cc || locality->admin1 != here.admin1)
            {
                if (auto same = pickLocality(rows, &here))
                    locality = same;
            }
        }

        // Park containment from the bbox gazetteer (Natural Earth parks).
        std::optional<Place> park;
        if (bounds_)
        {
            auto parks = bounds_->containing(lat, lon, "park");
            // Smallest box = most specific park.
            const LocationGeocoder::Entry *best = nullptr;
            double best_area = 0.0;
            for (const auto &e : parks)
            {
                const double area = (e.bounds.lat_max - e.bounds.lat_min) *
                                    (e.bounds.lon_max - e.bounds.lon_min);
                // Smallest box wins; on an identical box (the bundled file
                // lists "Yellowstone" and "Yellowstone NP" with one bbox)
                // the longer, more descriptive name does.
                if (!best || area < best_area ||
                    (area == best_area && e.name.size() > best->name.size()))
                {
                    best = &e;
                    best_area = area;
                }
            }
            if (best)
            {
                Place p;
                p.name = best->name;
                p.kind = "park";
                p.lat = (best->bounds.lat_min + best->bounds.lat_max) / 2.0;
                p.lon = (best->bounds.lon_min + best->bounds.lon_max) / 2.0;
                p.role = "park";
                p.source = "naturalearth";
                p.external_id = best->name;
                p.distance_m = 0.0;
                park = p;
            }
        }

        // Region / country codes come from the NEAREST resolved feature:
        // a waterfall 70 m away knows its state better than a town 24 km
        // across the border (Fairy Falls is in Wyoming; West Yellowstone,
        // its nearest town, is in Montana).
        const Place *admin_src = nullptr;
        if (locality)
            admin_src = &*locality;
        if (!landmarks.empty() && !landmarks.front().cc.empty() &&
            (!admin_src || landmarks.front().distance_m < admin_src->distance_m))
            admin_src = &landmarks.front();

        std::optional<Place> region, country;
        if (admin_src)
        {
            const std::string rname = regionName(admin_src->cc, admin_src->admin1);
            if (!rname.empty())
            {
                Place p;
                auto it = admin1_.find(admin_src->cc + "." + admin_src->admin1);
                p.geonameid = it->second.geonameid;
                p.name = rname;
                p.kind = "region";
                p.cc = admin_src->cc;
                p.admin1 = admin_src->admin1;
                p.role = "region";
                p.source = "geonames";
                p.external_id = p.geonameid > 0 ? std::to_string(p.geonameid)
                                                : "adm1:" + admin_src->cc + "." + admin_src->admin1;
                region = p;
            }
            const std::string cname = countryName(admin_src->cc);
            if (!cname.empty())
            {
                Place p;
                auto it = countries_.find(admin_src->cc);
                p.geonameid = it->second.geonameid;
                p.name = cname;
                p.kind = "country";
                p.cc = admin_src->cc;
                p.role = "country";
                p.source = "geonames";
                p.external_id = p.geonameid > 0 ? std::to_string(p.geonameid) : "cc:" + admin_src->cc;
                country = p;
            }
        }
        if (locality)
        {
            locality->role = "locality";
            const double town_radius = std::max(options_.in_town_m,
                                                16.0 * std::sqrt(static_cast<double>(locality->population)));
            res.locality_is_near = locality->distance_m > town_radius;
        }

        // Assemble rows + label (most specific first, at most 3 segments).
        std::vector<std::string> label_parts;
        auto addLabel = [&](const std::string &s) {
            if (s.empty() || label_parts.size() >= 3)
                return;
            if (std::find(label_parts.begin(), label_parts.end(), s) == label_parts.end())
                label_parts.push_back(s);
        };
        if (!landmarks.empty() && landmarks.front().distance_m <= options_.label_landmark_m)
            addLabel(landmarks.front().name);
        if (park)
            addLabel(park->name);
        if (locality && !park)
            addLabel((res.locality_is_near ? "near " : "") + locality->name);
        if (region)
            addLabel(region->name);
        if (country)
            addLabel(country->name);
        for (std::size_t i = 0; i < label_parts.size(); ++i)
            res.label += (i ? " · " : "") + label_parts[i];

        res.shore_distance_m = shoreDistance(lat, lon);

        for (auto &l : landmarks)
        {
            l.role = "landmark";
            res.places.push_back(l);
        }
        if (park)
            res.places.push_back(*park);
        if (locality)
            res.places.push_back(*locality);
        if (region)
            res.places.push_back(*region);
        if (country)
            res.places.push_back(*country);
        return res;
    }
}
