#include "database/places_ops.hpp"

#include "database/database_manager.hpp"
#include "database/db_retry.hpp"
#include "database/sql_constants.hpp"

#include <Poco/Data/RecordSet.h>
#include <Poco/Data/Session.h>
#include <Poco/Data/Statement.h>
#include <Poco/Exception.h>
#include <Poco/Logger.h>
#include <Poco/Nullable.h>

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace MediaDedup
{
    using Poco::Data::Keywords::into;
    using Poco::Data::Keywords::now;
    using Poco::Data::Keywords::use;

    namespace
    {
        constexpr const char *kCreatePlaces =
            "CREATE TABLE IF NOT EXISTS places ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  source TEXT NOT NULL,"
            "  external_id TEXT NOT NULL,"
            "  name TEXT NOT NULL,"
            "  kind TEXT NOT NULL,"
            "  lat REAL, lon REAL,"
            "  cc TEXT,"
            "  UNIQUE(source, external_id)"
            ")";

        constexpr const char *kCreateFilePlaces =
            "CREATE TABLE IF NOT EXISTS file_places ("
            "  file_id INTEGER NOT NULL,"
            "  place_id INTEGER NOT NULL,"
            "  role TEXT NOT NULL,"
            "  distance_m REAL NOT NULL DEFAULT 0,"
            "  source TEXT NOT NULL,"
            "  PRIMARY KEY (file_id, role, place_id)"
            ") WITHOUT ROWID";

        std::string lowerTrim(const std::string &s)
        {
            std::string out;
            out.reserve(s.size());
            for (unsigned char c : s)
                out.push_back(static_cast<char>(std::tolower(c)));
            auto b = out.find_first_not_of(" \t\r\n");
            if (b == std::string::npos)
                return "";
            auto e = out.find_last_not_of(" \t\r\n");
            return out.substr(b, e - b + 1);
        }
    }

    bool PlacesOps::ensureTable(DatabaseManager &db)
    {
        if (!db.ensureTableExists("places", kCreatePlaces) ||
            !db.ensureTableExists("file_places", kCreateFilePlaces))
            return false;
        // The FacesOps pattern: claim scanned_files early so the ALTERs
        // below have a table to alter on a fresh database.
        if (!db.ensureTableExists("scanned_files", SQL::kCreateScannedFilesTable))
            return false;

        for (const char *stmt_sql :
             {"ALTER TABLE scanned_files ADD COLUMN place_label TEXT",
              "ALTER TABLE scanned_files ADD COLUMN place_level INTEGER NOT NULL DEFAULT 0",
              // PLACE-4
              "ALTER TABLE scanned_files ADD COLUMN shore_distance_m REAL",
              "ALTER TABLE places ADD COLUMN fcode TEXT",
              "CREATE INDEX IF NOT EXISTS idx_places_fcode ON places(fcode)",
              "CREATE INDEX IF NOT EXISTS idx_places_name ON places(name COLLATE NOCASE)",
              "CREATE INDEX IF NOT EXISTS idx_file_places_place ON file_places(place_id)",
              // Sweep selector: GPS rows still at level 0.
              "CREATE INDEX IF NOT EXISTS idx_scanned_files_place_level ON scanned_files(place_level) WHERE exif_gps_lat IS NOT NULL"})
        {
            try
            {
                auto lease = db.acquireSessionLease();
                Poco::Data::Session &sess = lease.get();
                Poco::Data::Statement stmt(sess);
                stmt << std::string(stmt_sql), now;
            }
            catch (const Poco::Exception &e)
            {
                const std::string detail = e.displayText();
                if (detail.find("duplicate column") == std::string::npos)
                    Poco::Logger::get("PlacesOps").warning("Migration failed: %s — %s",
                                                           std::string(stmt_sql), detail);
            }
            catch (const std::exception &e)
            {
                Poco::Logger::get("PlacesOps").warning("Migration failed: %s — %s",
                                                       std::string(stmt_sql), std::string(e.what()));
            }
        }
        // Every write and the day-view SELECT read these columns; fail
        // initialization loudly instead of per call (the FacesOps probe).
        try
        {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            Poco::Data::Statement probe(sess);
            probe << "SELECT place_label, place_level, shore_distance_m FROM scanned_files LIMIT 0", now;
            Poco::Data::Statement probe2(sess);
            probe2 << "SELECT fcode FROM places LIMIT 0", now;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("PlacesOps").error(
                "place columns missing after migration: %s", std::string(e.what()));
            return false;
        }
        return true;
    }

    namespace
    {
        // Insert-or-find inside the caller's session/transaction. Unchanged
        // rows are not rewritten (DO NOTHING + SELECT) so a re-resolve of a
        // file that already references its places dirties no pages.
        int upsertPlaceIn(Poco::Data::Session &sess, const PlacesOps::PlaceRow &place)
        {
            if (place.source.empty() || place.external_id.empty() || place.name.empty())
                return 0;
            std::string source = place.source, ext = place.external_id;
            std::string name = place.name, kind = place.kind, cc = place.cc;
            double lat = place.lat, lon = place.lon;
            Poco::Nullable<std::string> fcode;
            if (!place.fcode.empty())
                fcode = place.fcode;
            int id = 0;
            Poco::Data::Statement ins(sess);
            ins << "INSERT INTO places (source, external_id, name, kind, lat, lon, cc, fcode) "
                   "VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
                   "ON CONFLICT(source, external_id) DO UPDATE SET "
                   "name = excluded.name, kind = excluded.kind, "
                   "lat = excluded.lat, lon = excluded.lon, cc = excluded.cc, fcode = excluded.fcode "
                   "WHERE name IS NOT excluded.name OR kind IS NOT excluded.kind "
                   "OR lat IS NOT excluded.lat OR lon IS NOT excluded.lon "
                   "OR cc IS NOT excluded.cc OR fcode IS NOT excluded.fcode",
                use(source), use(ext), use(name), use(kind), use(lat), use(lon), use(cc), use(fcode), now;
            Poco::Data::Statement sel(sess);
            sel << "SELECT id FROM places WHERE source = ? AND external_id = ?",
                use(source), use(ext), into(id), now;
            return id;
        }
    }

    int PlacesOps::upsertPlace(DatabaseManager &db, const PlaceRow &place)
    {
        int id = 0;
        const bool ok = DbRetry::withBusyRetry("PlacesOps", "upsertPlace", [&]() {
            auto lease = db.acquireSessionLease();
            id = upsertPlaceIn(lease.get(), place);
            return id > 0;
        });
        return ok ? id : 0;
    }

    int PlacesOps::deleteFilePlacesForFiles(Poco::Data::Session &sess, const std::vector<int> &file_ids)
    {
        if (file_ids.empty())
            return 0;
        // A database that never ran ensureTable has no file_places; the
        // caller's delete transaction must not die on that.
        {
            int n = 0;
            sess << "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='file_places'",
                into(n), now;
            if (n == 0)
                return 0;
        }
        // One statement per file: SQLITE_MAX_VARIABLE_NUMBER caps an IN
        // list, and the PK (file_id, ...) makes each seek cheap.
        int deleted = 0;
        for (int file_id : file_ids)
        {
            if (file_id <= 0)
                continue;
            int fid = file_id;
            Poco::Data::Statement del(sess);
            del << "DELETE FROM file_places WHERE file_id = ?", use(fid);
            deleted += static_cast<int>(del.execute());
        }
        return deleted;
    }

    int PlacesOps::deleteOrphanPlaces(DatabaseManager &db)
    {
        int deleted = 0;
        DbRetry::withBusyRetry("PlacesOps", "deleteOrphanPlaces", [&]() {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            Poco::Data::Statement del(sess);
            del << "DELETE FROM places WHERE id NOT IN (SELECT DISTINCT place_id FROM file_places)";
            deleted = static_cast<int>(del.execute());
            return true;
        });
        return deleted;
    }

    bool PlacesOps::setFilePlaces(DatabaseManager &db, int file_id,
                                  const std::vector<FilePlace> &places,
                                  const std::string &label, int level,
                                  std::optional<double> shore_distance_m)
    {
        if (file_id <= 0)
            return false;
        for (const auto &fp : places)
            if (fp.place.source.empty() || fp.place.external_id.empty() || fp.place.name.empty())
                return false; // nothing is written for a malformed resolution
        // One transaction for everything: the place upserts, the
        // file_places replacement and the scanned_files update. This runs
        // once per GPS photo inside the scan workers, so it must be one
        // writer turn, not one per place row.
        return DbRetry::withBusyRetry("PlacesOps", "setFilePlaces", [&]() {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            int fid = file_id;
            sess.begin();
            try
            {
                struct Resolved { int place_id; std::string role; double distance_m; std::string source; };
                std::vector<Resolved> rows;
                rows.reserve(places.size());
                for (const auto &fp : places)
                {
                    const int pid = upsertPlaceIn(sess, fp.place);
                    if (pid <= 0) // withBusyRetry only sees exceptions
                        throw std::runtime_error("place upsert returned no id");
                    rows.push_back({pid, fp.role, fp.distance_m, fp.place.source});
                }
                Poco::Data::Statement del(sess);
                del << "DELETE FROM file_places WHERE file_id = ?", use(fid), now;
                for (auto &r : rows)
                {
                    Poco::Data::Statement ins(sess);
                    ins << "INSERT OR REPLACE INTO file_places (file_id, place_id, role, distance_m, source) "
                           "VALUES (?, ?, ?, ?, ?)",
                        use(fid), use(r.place_id), use(r.role), use(r.distance_m), use(r.source), now;
                }
                Poco::Nullable<std::string> lbl;
                if (!label.empty())
                    lbl = label;
                Poco::Nullable<double> shore;
                if (shore_distance_m)
                    shore = *shore_distance_m;
                int lvl = level;
                Poco::Data::Statement upd(sess);
                upd << "UPDATE scanned_files SET place_label = ?, place_level = ?, shore_distance_m = ? "
                       "WHERE id = ?",
                    use(lbl), use(lvl), use(shore), use(fid), now;
                sess.commit();
            }
            catch (...)
            {
                sess.rollback();
                throw;
            }
            return true;
        });
    }

    std::vector<int> PlacesOps::placeIdsByName(DatabaseManager &db, const std::string &name)
    {
        std::vector<int> ids;
        const std::string n = lowerTrim(name);
        if (n.empty())
            return ids;
        try
        {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            std::string exact = n;
            // Word-boundary containment: pad both sides with spaces so
            // "yellowstone" matches "Yellowstone National Park" and "West
            // Yellowstone" but never "Yellowstoned".
            std::string padded = "% " + n + " %";
            Poco::Data::Statement stmt(sess);
            stmt << "SELECT DISTINCT p.id FROM places p "
                    "WHERE EXISTS (SELECT 1 FROM file_places fp WHERE fp.place_id = p.id) "
                    "AND (lower(p.name) = ? OR (' ' || lower(p.name) || ' ') LIKE ?)",
                use(exact), use(padded), into(ids), now;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("PlacesOps").error("placeIdsByName: %s", std::string(e.what()));
            ids.clear();
        }
        std::sort(ids.begin(), ids.end());
        return ids;
    }

    std::vector<PlacesOps::Unresolved> PlacesOps::listUnresolved(DatabaseManager &db, int min_level, int limit)
    {
        std::vector<Unresolved> out;
        try
        {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            int lvl = min_level;
            int lim = limit > 0 ? limit : -1;
            Poco::Data::Statement stmt(sess);
            stmt << "SELECT id, exif_gps_lat, exif_gps_lon FROM scanned_files "
                    "WHERE exif_gps_lat IS NOT NULL AND exif_gps_lon IS NOT NULL "
                    "AND place_level < ? ORDER BY id LIMIT ?",
                use(lvl), use(lim);
            stmt.execute();
            Poco::Data::RecordSet rs(stmt);
            for (bool more = rs.moveFirst(); more; more = rs.moveNext())
            {
                Unresolved u;
                u.file_id = rs[0].convert<int>();
                u.lat = rs[1].convert<double>();
                u.lon = rs[2].convert<double>();
                out.push_back(u);
            }
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("PlacesOps").error("listUnresolved: %s", std::string(e.what()));
        }
        return out;
    }

    int PlacesOps::countUnresolved(DatabaseManager &db, int min_level)
    {
        int count = 0;
        try
        {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            int lvl = min_level;
            Poco::Data::Statement stmt(sess);
            stmt << "SELECT COUNT(*) FROM scanned_files "
                    "WHERE exif_gps_lat IS NOT NULL AND exif_gps_lon IS NOT NULL AND place_level < ?",
                use(lvl), into(count), now;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("PlacesOps").error("countUnresolved: %s", std::string(e.what()));
        }
        return count;
    }

    std::vector<PlacesOps::FilePlaceRow> PlacesOps::filePlaces(DatabaseManager &db, int file_id)
    {
        std::vector<FilePlaceRow> out;
        try
        {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            int fid = file_id;
            Poco::Data::Statement stmt(sess);
            stmt << "SELECT fp.place_id, fp.role, fp.distance_m, fp.source, p.name, p.kind "
                    "FROM file_places fp JOIN places p ON p.id = fp.place_id "
                    "WHERE fp.file_id = ? ORDER BY fp.distance_m, fp.role",
                use(fid);
            stmt.execute();
            Poco::Data::RecordSet rs(stmt);
            for (bool more = rs.moveFirst(); more; more = rs.moveNext())
            {
                FilePlaceRow r;
                r.place_id = rs[0].convert<int>();
                r.role = rs[1].convert<std::string>();
                r.distance_m = rs[2].convert<double>();
                r.source = rs[3].convert<std::string>();
                r.name = rs[4].convert<std::string>();
                r.kind = rs[5].convert<std::string>();
                out.push_back(r);
            }
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("PlacesOps").error("filePlaces: %s", std::string(e.what()));
        }
        return out;
    }

    int PlacesOps::resetAll(DatabaseManager &db)
    {
        int changed = 0;
        DbRetry::withBusyRetry("PlacesOps", "resetAll", [&]() {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            sess.begin();
            try
            {
                sess << "DELETE FROM file_places", now;
                sess << "DELETE FROM places", now;
                sess << "UPDATE scanned_files SET place_label = NULL, place_level = 0, "
                        "shore_distance_m = NULL WHERE place_level <> 0", now;
                Poco::Data::Statement cnt(sess);
                cnt << "SELECT changes()", into(changed), now;
                sess.commit();
            }
            catch (...)
            {
                sess.rollback();
                throw;
            }
            return true;
        });
        return changed;
    }
}
