#include "database/faces_ops.hpp"

#include "database/database_manager.hpp"
#include "database/db_retry.hpp"

#include <Poco/Data/RecordSet.h>
#include <Poco/Data/Session.h>
#include <Poco/Data/Statement.h>
#include <Poco/Nullable.h>
#include <Poco/Logger.h>

namespace MediaDedup
{
    using Poco::Data::Keywords::into;
    using Poco::Data::Keywords::now;
    using Poco::Data::Keywords::use;

    namespace
    {
        constexpr const char *kCreateFaces =
            "CREATE TABLE IF NOT EXISTS faces (\n"
            "    id INTEGER PRIMARY KEY AUTOINCREMENT,\n"
            "    file_id INTEGER NOT NULL,\n"
            "    bbox_x REAL NOT NULL,\n"
            "    bbox_y REAL NOT NULL,\n"
            "    bbox_w REAL NOT NULL,\n"
            "    bbox_h REAL NOT NULL,\n"
            "    quality REAL NOT NULL DEFAULT 0,\n"
            "    embedding BLOB,\n"
            "    cluster_id INTEGER,\n"
            "    person_id INTEGER,\n"
            "    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP\n"
            ")";

        constexpr const char *kCreateFaceClusters =
            "CREATE TABLE IF NOT EXISTS face_clusters (\n"
            "    id INTEGER PRIMARY KEY AUTOINCREMENT,\n"
            "    centroid BLOB,\n"
            "    size INTEGER NOT NULL DEFAULT 0,\n"
            "    cover_face_id INTEGER,\n"
            "    person_id INTEGER\n"
            ")";

        constexpr const char *kCreatePersons =
            "CREATE TABLE IF NOT EXISTS persons (\n"
            "    id INTEGER PRIMARY KEY AUTOINCREMENT,\n"
            "    name TEXT NOT NULL UNIQUE COLLATE NOCASE,\n"
            "    cover_face_id INTEGER,\n"
            "    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP\n"
            ")";
    }

    bool FacesOps::ensureTable(DatabaseManager &db)
    {
        if (!db.ensureTableExists("faces", kCreateFaces) ||
            !db.ensureTableExists("face_clusters", kCreateFaceClusters) ||
            !db.ensureTableExists("persons", kCreatePersons))
            return false;

        // scanned_files migrations (each ALTER adds one column; a failure
        // means the column already exists).
        for (const char *alter :
             {"ALTER TABLE scanned_files ADD COLUMN face_scan_version INTEGER NOT NULL DEFAULT 0",
              "ALTER TABLE scanned_files ADD COLUMN face_count INTEGER"})
        {
            try
            {
                auto lease = db.acquireSessionLease();
                Poco::Data::Session &sess = lease.get();
                Poco::Data::Statement stmt(sess);
                stmt << std::string(alter), now;
                Poco::Logger::get("FacesOps").information("Applied migration: %s", std::string(alter));
            }
            catch (const std::exception &)
            {
                // Column already exists.
            }
        }

        try
        {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            for (const char *idx :
                 {"CREATE INDEX IF NOT EXISTS idx_faces_file_id ON faces(file_id)",
                  "CREATE INDEX IF NOT EXISTS idx_faces_cluster_id ON faces(cluster_id)",
                  "CREATE INDEX IF NOT EXISTS idx_faces_person_id ON faces(person_id)",
                  // The sweep selector's partial index: eligible files only.
                  "CREATE INDEX IF NOT EXISTS idx_scanned_files_face_scan ON scanned_files(face_scan_version) "
                  "WHERE processed >= 2 AND is_video = 0"})
            {
                Poco::Data::Statement stmt(sess);
                stmt << std::string(idx), now;
            }
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("FacesOps").warning("Index creation: %s", std::string(e.what()));
        }
        return true;
    }

    std::vector<FacesOps::SweepCandidate> FacesOps::listSweepCandidates(DatabaseManager &db, int limit)
    {
        std::vector<SweepCandidate> out;
        try
        {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            int version = kFaceScanVersion;
            Poco::Data::Statement stmt(sess);
            stmt << "SELECT id, file_path FROM scanned_files "
                    "WHERE processed >= 2 AND is_video = 0 AND face_scan_version < ? "
                    "ORDER BY id LIMIT ?",
                use(version), use(limit);
            stmt.execute();
            Poco::Data::RecordSet rs(stmt);
            for (bool more = rs.moveFirst(); more; more = rs.moveNext())
                out.push_back({rs[0].convert<int>(), rs[1].convert<std::string>()});
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("FacesOps").error("listSweepCandidates: %s", std::string(e.what()));
        }
        return out;
    }

    int FacesOps::insertFace(DatabaseManager &db, const FaceRow &row)
    {
        int id = 0;
        DbRetry::withBusyRetry("FacesOps", "insertFace", [&]() {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            FaceRow r = row; // Poco use() needs non-const lvalues
            Poco::Data::BLOB blob(row.embedding);
            Poco::Data::Statement stmt(sess);
            stmt << "INSERT INTO faces (file_id, bbox_x, bbox_y, bbox_w, bbox_h, quality, embedding) "
                    "VALUES (?, ?, ?, ?, ?, ?, ?)",
                use(r.file_id), use(r.bbox_x), use(r.bbox_y), use(r.bbox_w), use(r.bbox_h),
                use(r.quality), use(blob), now;

            Poco::Data::Statement sel(sess);
            sel << "SELECT last_insert_rowid()", into(id), now;
            return true;
        });
        return id;
    }

    bool FacesOps::markFileScanned(DatabaseManager &db, int file_id, int face_count)
    {
        // Losing this to lock contention would re-scan the file forever.
        return DbRetry::withBusyRetry("FacesOps", "markFileScanned", [&]() {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            int fid = file_id, version = kFaceScanVersion;
            Poco::Nullable<int> count;
            if (face_count >= 0)
                count = face_count;
            Poco::Data::Statement stmt(sess);
            stmt << "UPDATE scanned_files SET face_scan_version = ?, face_count = ? WHERE id = ?",
                use(version), use(count), use(fid), now;
            return true;
        });
    }

    bool FacesOps::deleteFacesForFile(DatabaseManager &db, int file_id)
    {
        return DbRetry::withBusyRetry("FacesOps", "deleteFacesForFile", [&]() {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            int fid = file_id;
            Poco::Data::Statement stmt(sess);
            stmt << "DELETE FROM faces WHERE file_id = ?", use(fid), now;
            return true;
        });
    }

    std::vector<FacesOps::FaceRow> FacesOps::listFacesForFile(DatabaseManager &db, int file_id)
    {
        std::vector<FaceRow> out;
        try
        {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            int fid = file_id;
            Poco::Data::Statement stmt(sess);
            stmt << "SELECT id, file_id, bbox_x, bbox_y, bbox_w, bbox_h, quality, embedding "
                    "FROM faces WHERE file_id = ? ORDER BY id",
                use(fid);
            stmt.execute();
            Poco::Data::RecordSet rs(stmt);
            for (bool more = rs.moveFirst(); more; more = rs.moveNext())
            {
                FaceRow r;
                r.id = rs[0].convert<int>();
                r.file_id = rs[1].convert<int>();
                r.bbox_x = rs[2].convert<double>();
                r.bbox_y = rs[3].convert<double>();
                r.bbox_w = rs[4].convert<double>();
                r.bbox_h = rs[5].convert<double>();
                r.quality = rs[6].convert<double>();
                if (!rs[7].isEmpty())
                {
                    auto blob = rs[7].extract<Poco::Data::BLOB>();
                    r.embedding.assign(blob.rawContent(), blob.rawContent() + blob.size());
                }
                out.push_back(std::move(r));
            }
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("FacesOps").error("listFacesForFile: %s", std::string(e.what()));
        }
        return out;
    }
}
