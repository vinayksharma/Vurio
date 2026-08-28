#include "database/faces_ops.hpp"

#include "database/database_manager.hpp"
#include "database/db_retry.hpp"
#include "database/sql_constants.hpp"

#include <Poco/Data/RecordSet.h>
#include <Poco/Data/Session.h>
#include <Poco/Data/Statement.h>
#include <Poco/Exception.h>
#include <Poco/Nullable.h>
#include <Poco/Logger.h>
#include <algorithm>
#include <cmath>
#include <cstring>

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

        // On a fresh database this runs before FilesManager creates
        // scanned_files, and ALTERing a missing table fails — silently, until
        // the sweep's queries error on every tick. Claim the bare table here
        // (the duplicate_groups_ops.cpp pattern): ScannedFilesOps::ensureTable
        // opens with this same idempotent call and layers its migrations on
        // top, so creating it early costs nothing.
        if (!db.ensureTableExists("scanned_files", SQL::kCreateScannedFilesTable))
            return false;

        // scanned_files migrations (each ALTER adds one column; the expected
        // failure is "duplicate column" on every boot after the first).
        for (const char *alter :
             {"ALTER TABLE scanned_files ADD COLUMN face_scan_version INTEGER NOT NULL DEFAULT 0",
              "ALTER TABLE scanned_files ADD COLUMN face_count INTEGER",
              // FACE-2: cover election needs the cover's quality without a join.
              "ALTER TABLE face_clusters ADD COLUMN cover_quality REAL NOT NULL DEFAULT 0",
              // FACE-3: user-hidden clusters (people they don't care about).
              "ALTER TABLE face_clusters ADD COLUMN hidden INTEGER NOT NULL DEFAULT 0",
              // FACE-3b: "not this person" constraints. CREATE-as-ALTER is a
              // no-op when present, keeping one migration list.
              "CREATE TABLE IF NOT EXISTS face_cannot_link ("
              "face_id INTEGER NOT NULL, cluster_id INTEGER NOT NULL, "
              "PRIMARY KEY (face_id, cluster_id)) WITHOUT ROWID",
              // FACE-5: alternative names ("mom" -> Alice). Globally unique:
              // an alias resolves to exactly one person.
              "CREATE TABLE IF NOT EXISTS person_aliases ("
              "alias TEXT NOT NULL PRIMARY KEY COLLATE NOCASE, "
              "person_id INTEGER NOT NULL) WITHOUT ROWID",
              // FACE-6: user-pinned cover faces survive automatic election.
              "ALTER TABLE face_clusters ADD COLUMN cover_pinned INTEGER NOT NULL DEFAULT 0",
              // FACE-7b: up to K exemplar faces per cluster; matching scores
              // against the best exemplar instead of one blended centroid.
              "CREATE TABLE IF NOT EXISTS face_cluster_exemplars ("
              "cluster_id INTEGER NOT NULL, face_id INTEGER NOT NULL, "
              "PRIMARY KEY (cluster_id, face_id)) WITHOUT ROWID"})
        {
            try
            {
                auto lease = db.acquireSessionLease();
                Poco::Data::Session &sess = lease.get();
                Poco::Data::Statement stmt(sess);
                stmt << std::string(alter), now;
                Poco::Logger::get("FacesOps").information("Applied migration: %s", std::string(alter));
            }
            catch (const Poco::Exception &e)
            {
                // "duplicate column" is the normal re-boot case; anything
                // else (locked, missing table) must not be masked as it —
                // that silence is how the fresh-DB boot shipped broken.
                const std::string detail = e.displayText();
                if (detail.find("duplicate column") == std::string::npos)
                    Poco::Logger::get("FacesOps").warning("Migration failed: %s — %s",
                                                          std::string(alter), detail);
            }
            catch (const std::exception &e)
            {
                Poco::Logger::get("FacesOps").warning("Migration failed: %s — %s",
                                                      std::string(alter), std::string(e.what()));
            }
        }

        // The sweep is dead without these columns; fail initialization loudly
        // instead of erroring on every tick.
        try
        {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            Poco::Data::Statement stmt(sess);
            stmt << "SELECT face_scan_version, face_count FROM scanned_files LIMIT 0", now;
        }
        catch (const std::exception &)
        {
            Poco::Logger::get("FacesOps").error(
                "scanned_files is missing the face sweep columns after migration");
            return false;
        }

        try
        {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            for (const char *idx :
                 {"CREATE INDEX IF NOT EXISTS idx_faces_file_id ON faces(file_id)",
                  // (cluster_id, file_id) so listClusterInfos' per-cluster
                  // COUNT(DISTINCT file_id) is a covering index SEARCH. On
                  // cluster_id alone the planner fell back to a full faces
                  // scan PER CLUSTER — 7k clusters x 75k faces made the
                  // PEOPLE tab take minutes on a cold cache. Supersedes the
                  // old idx_faces_cluster_id (left prefix), which is dropped.
                  "DROP INDEX IF EXISTS idx_faces_cluster_id",
                  "CREATE INDEX IF NOT EXISTS idx_faces_cluster_file ON faces(cluster_id, file_id)",
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

    // ---- FACE-2: clusters ------------------------------------------------

    namespace
    {
        std::vector<float> blobToFloats(const Poco::Data::BLOB &blob)
        {
            std::vector<float> out(blob.size() / sizeof(float));
            std::memcpy(out.data(), blob.rawContent(), out.size() * sizeof(float));
            return out;
        }

        Poco::Data::BLOB floatsToBlob(const std::vector<float> &v)
        {
            return Poco::Data::BLOB(
                reinterpret_cast<const unsigned char *>(v.data()), v.size() * sizeof(float));
        }
    }

    namespace
    {
        // FACE-7: shared by exemplar selection and fit scoring.
        double cosineF(const std::vector<float> &a, const std::vector<float> &b)
        {
            if (a.size() != b.size() || a.empty())
                return -1.0;
            double dot = 0;
            for (std::size_t i = 0; i < a.size(); ++i)
                dot += static_cast<double>(a[i]) * b[i];
            return dot; // unit-normalized inputs
        }

        struct MemberFace
        {
            int id = 0;
            double quality = 0;
            std::vector<float> embedding;
        };

        std::vector<MemberFace> loadMembers(DatabaseManager &db, int cluster_id)
        {
            std::vector<MemberFace> out;
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            int cid = cluster_id;
            Poco::Data::Statement stmt(sess);
            stmt << "SELECT id, quality, embedding FROM faces "
                    "WHERE cluster_id = ? AND embedding IS NOT NULL",
                use(cid);
            stmt.execute();
            Poco::Data::RecordSet rs(stmt);
            for (bool more = rs.moveFirst(); more; more = rs.moveNext())
            {
                MemberFace m;
                m.id = rs[0].convert<int>();
                m.quality = rs[1].convert<double>();
                m.embedding = blobToFloats(rs[2].extract<Poco::Data::BLOB>());
                out.push_back(std::move(m));
            }
            return out;
        }
    }


    std::vector<FacesOps::ClusterRow> FacesOps::listClusters(DatabaseManager &db)
    {
        std::vector<ClusterRow> out;
        try
        {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            Poco::Data::Statement stmt(sess);
            stmt << "SELECT id, centroid, size, COALESCE(cover_face_id, 0), cover_quality, "
                    "COALESCE(person_id, 0), cover_pinned FROM face_clusters ORDER BY id";
            stmt.execute();
            Poco::Data::RecordSet rs(stmt);
            for (bool more = rs.moveFirst(); more; more = rs.moveNext())
            {
                ClusterRow r;
                r.id = rs[0].convert<int>();
                if (!rs[1].isEmpty())
                    r.centroid = blobToFloats(rs[1].extract<Poco::Data::BLOB>());
                r.size = rs[2].convert<int>();
                r.cover_face_id = rs[3].convert<int>();
                r.cover_quality = rs[4].convert<double>();
                r.person_id = rs[5].convert<int>();
                r.cover_pinned = rs[6].convert<int>() != 0;
                out.push_back(std::move(r));
            }
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("FacesOps").error("listClusters: %s", std::string(e.what()));
        }
        return out;
    }

    int FacesOps::insertCluster(DatabaseManager &db, const ClusterRow &row)
    {
        int id = 0;
        DbRetry::withBusyRetry("FacesOps", "insertCluster", [&]() {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            Poco::Data::BLOB blob = floatsToBlob(row.centroid);
            int size = row.size, cover = row.cover_face_id, pinned = row.cover_pinned ? 1 : 0;
            double cq = row.cover_quality;
            Poco::Data::Statement stmt(sess);
            stmt << "INSERT INTO face_clusters (centroid, size, cover_face_id, cover_quality, "
                    "cover_pinned) VALUES (?, ?, ?, ?, ?)",
                use(blob), use(size), use(cover), use(cq), use(pinned), now;
            Poco::Data::Statement sel(sess);
            sel << "SELECT last_insert_rowid()", into(id), now;
            return true;
        });
        return id;
    }

    bool FacesOps::updateCluster(DatabaseManager &db, const ClusterRow &row)
    {
        return DbRetry::withBusyRetry("FacesOps", "updateCluster", [&]() {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            Poco::Data::BLOB blob = floatsToBlob(row.centroid);
            int id = row.id, size = row.size, cover = row.cover_face_id,
                pinned = row.cover_pinned ? 1 : 0;
            double cq = row.cover_quality;
            Poco::Data::Statement stmt(sess);
            stmt << "UPDATE face_clusters SET centroid = ?, size = ?, cover_face_id = ?, "
                    "cover_quality = ?, cover_pinned = ? WHERE id = ?",
                use(blob), use(size), use(cover), use(cq), use(pinned), use(id), now;
            return true;
        });
    }

    bool FacesOps::deleteCluster(DatabaseManager &db, int cluster_id)
    {
        return DbRetry::withBusyRetry("FacesOps", "deleteCluster", [&]() {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            int id = cluster_id;
            Poco::Data::Statement stmt(sess);
            stmt << "DELETE FROM face_clusters WHERE id = ?", use(id), now;
            // FACE-7b: exemplar rows die with the cluster.
            Poco::Data::Statement dele(sess);
            dele << "DELETE FROM face_cluster_exemplars WHERE cluster_id = ?", use(id), now;
            return true;
        });
    }

    bool FacesOps::setFaceCluster(DatabaseManager &db, int face_id, int cluster_id)
    {
        return DbRetry::withBusyRetry("FacesOps", "setFaceCluster", [&]() {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            int fid = face_id, cid = cluster_id;
            Poco::Data::Statement stmt(sess);
            stmt << "UPDATE faces SET cluster_id = ? WHERE id = ?", use(cid), use(fid), now;
            return true;
        });
    }

    bool FacesOps::moveClusterMembers(DatabaseManager &db, int from, int to)
    {
        return DbRetry::withBusyRetry("FacesOps", "moveClusterMembers", [&]() {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            int f = from, t = to;
            Poco::Data::Statement stmt(sess);
            stmt << "UPDATE faces SET cluster_id = ? WHERE cluster_id = ?", use(t), use(f), now;
            return true;
        });
    }

    std::vector<FacesOps::UnclusteredFace> FacesOps::listUnclusteredFaces(DatabaseManager &db,
                                                                          double min_quality, int limit)
    {
        std::vector<UnclusteredFace> out;
        try
        {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            double q = min_quality;
            int lim = limit;
            Poco::Data::Statement stmt(sess);
            stmt << "SELECT id, quality, embedding FROM faces "
                    "WHERE cluster_id IS NULL AND embedding IS NOT NULL AND quality >= ? "
                    "ORDER BY id LIMIT ?",
                use(q), use(lim);
            stmt.execute();
            Poco::Data::RecordSet rs(stmt);
            for (bool more = rs.moveFirst(); more; more = rs.moveNext())
            {
                UnclusteredFace f;
                f.face_id = rs[0].convert<int>();
                f.quality = rs[1].convert<double>();
                f.embedding = blobToFloats(rs[2].extract<Poco::Data::BLOB>());
                out.push_back(std::move(f));
            }
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("FacesOps").error("listUnclusteredFaces: %s", std::string(e.what()));
        }
        return out;
    }

    bool FacesOps::scanProgress(DatabaseManager &db, int &total, int &completed)
    {
        total = 0;
        completed = 0;
        try
        {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            int version = kFaceScanVersion;
            Poco::Data::Statement stmt(sess);
            stmt << "SELECT COUNT(*), "
                    "COALESCE(SUM(CASE WHEN face_scan_version >= ? THEN 1 ELSE 0 END), 0) "
                    "FROM scanned_files WHERE processed >= 2 AND is_video = 0",
                use(version), into(total), into(completed), now;
            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("FacesOps").error("scanProgress: %s", std::string(e.what()));
            return false;
        }
    }

    // ---- FACE-3: People tab ----------------------------------------------

    std::vector<FacesOps::ClusterInfo> FacesOps::listClusterInfos(DatabaseManager &db,
                                                                  bool include_hidden)
    {
        std::vector<ClusterInfo> out;
        try
        {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            Poco::Data::Statement stmt(sess);
            std::string sql =
                "SELECT c.id, COALESCE(p.name, ''), c.size, "
                "  (SELECT COUNT(DISTINCT f.file_id) FROM faces f WHERE f.cluster_id = c.id), "
                "  COALESCE(sf.file_path, ''), "
                "  COALESCE(cf.bbox_x, 0), COALESCE(cf.bbox_y, 0), "
                "  COALESCE(cf.bbox_w, 0), COALESCE(cf.bbox_h, 0), c.hidden, "
                "  COALESCE(c.person_id, 0), COALESCE(c.cover_face_id, 0), c.cover_pinned "
                "FROM face_clusters c "
                "LEFT JOIN persons p ON p.id = c.person_id "
                "LEFT JOIN faces cf ON cf.id = c.cover_face_id "
                "LEFT JOIN scanned_files sf ON sf.id = cf.file_id ";
            if (!include_hidden)
                sql += "WHERE c.hidden = 0 ";
            sql += "ORDER BY c.size DESC, c.id";
            stmt << sql;
            stmt.execute();
            Poco::Data::RecordSet rs(stmt);
            for (bool more = rs.moveFirst(); more; more = rs.moveNext())
            {
                ClusterInfo c;
                c.id = rs[0].convert<int>();
                c.name = rs[1].convert<std::string>();
                c.face_count = rs[2].convert<int>();
                c.file_count = rs[3].convert<int>();
                c.cover_file_path = rs[4].convert<std::string>();
                c.cover_x = rs[5].convert<double>();
                c.cover_y = rs[6].convert<double>();
                c.cover_w = rs[7].convert<double>();
                c.cover_h = rs[8].convert<double>();
                c.hidden = rs[9].convert<int>() != 0;
                c.person_id = rs[10].convert<int>();
                c.cover_face_id = rs[11].convert<int>();
                c.cover_pinned = rs[12].convert<int>() != 0;
                out.push_back(std::move(c));
            }
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("FacesOps").error("listClusterInfos: %s", std::string(e.what()));
        }
        for (auto &c : out)
            if (c.person_id > 0)
                c.aliases = listPersonAliases(db, c.person_id);
        return out;
    }

    bool FacesOps::nameCluster(DatabaseManager &db, int cluster_id, const std::string &name)
    {
        return DbRetry::withBusyRetry("FacesOps", "nameCluster", [&]() {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            int cid = cluster_id;

            int old_person = 0;
            {
                Poco::Data::Statement sel(sess);
                sel << "SELECT COALESCE(person_id, 0) FROM face_clusters WHERE id = ?",
                    use(cid), into(old_person), now;
            }

            int person_id = 0;
            if (!name.empty())
            {
                std::string n = name;
                Poco::Data::Statement ins(sess);
                ins << "INSERT INTO persons (name) VALUES (?) "
                       "ON CONFLICT(name) DO UPDATE SET name = excluded.name",
                    use(n), now;
                Poco::Data::Statement sel(sess);
                sel << "SELECT id FROM persons WHERE name = ? COLLATE NOCASE",
                    use(n), into(person_id), now;
                if (person_id <= 0)
                    return false;
            }

            {
                Poco::Nullable<int> pid;
                if (person_id > 0)
                    pid = person_id;
                Poco::Data::Statement upd(sess);
                upd << "UPDATE face_clusters SET person_id = ? WHERE id = ?",
                    use(pid), use(cid), now;
                Poco::Data::Statement updf(sess);
                updf << "UPDATE faces SET person_id = ? WHERE cluster_id = ?",
                    use(pid), use(cid), now;
            }

            // The old person may be orphaned now (a rename away from it).
            if (old_person > 0 && old_person != person_id)
            {
                Poco::Data::Statement del(sess);
                int op = old_person;
                del << "DELETE FROM persons WHERE id = ? AND NOT EXISTS "
                       "(SELECT 1 FROM face_clusters WHERE person_id = ?)",
                    use(op), use(op), now;
                // FACE-5: a removed person takes its aliases with it.
                Poco::Data::Statement dela(sess);
                dela << "DELETE FROM person_aliases WHERE person_id = ? AND NOT EXISTS "
                        "(SELECT 1 FROM persons WHERE id = ?)",
                    use(op), use(op), now;
            }
            return true;
        });
    }

    bool FacesOps::mergeClusters(DatabaseManager &db, int target_id,
                                 const std::vector<int> &source_ids,
                                 double purge_cutoff, int *purged)
    {
        if (purged)
            *purged = 0;
        // Combine centroids size-weighted in C++ (they are small blobs).
        auto clusters = listClusters(db);
        const ClusterRow *target = nullptr;
        std::vector<const ClusterRow *> sources;
        for (const auto &c : clusters)
        {
            if (c.id == target_id)
                target = &c;
            else if (std::find(source_ids.begin(), source_ids.end(), c.id) != source_ids.end())
                sources.push_back(&c);
        }
        if (!target || sources.empty())
            return false;

        ClusterRow merged = *target;
        std::vector<double> acc(merged.centroid.begin(), merged.centroid.end());
        for (auto &v : acc)
            v *= merged.size;
        for (const auto *s : sources)
        {
            if (s->centroid.size() != acc.size())
                continue;
            for (std::size_t i = 0; i < acc.size(); ++i)
                acc[i] += static_cast<double>(s->centroid[i]) * s->size;
            merged.size += s->size;
            // FACE-6 cover election: a pin is a statement about the person,
            // so it survives the merge. Pinned beats unpinned; both pinned
            // keeps the target's (the cluster the user chose); both
            // unpinned falls back to quality.
            if (!merged.cover_pinned)
            {
                if (s->cover_pinned)
                {
                    merged.cover_quality = s->cover_quality;
                    merged.cover_face_id = s->cover_face_id;
                    merged.cover_pinned = true;
                }
                else if (s->cover_quality > merged.cover_quality)
                {
                    merged.cover_quality = s->cover_quality;
                    merged.cover_face_id = s->cover_face_id;
                }
            }
            if (merged.person_id == 0 && s->person_id != 0)
                merged.person_id = s->person_id;
        }
        double norm = 0;
        for (double v : acc)
            norm += v * v;
        if (norm > 0)
        {
            const double inv = 1.0 / std::sqrt(norm);
            for (std::size_t i = 0; i < acc.size(); ++i)
                merged.centroid[i] = static_cast<float>(acc[i] * inv);
        }

        for (const auto *s : sources)
        {
            moveClusterMembers(db, s->id, target_id);
            // FACE-7a: "not the person of the source" means "not the person
            // of the target" once they are one identity — the constraint
            // follows the merge here exactly as it does in consolidation.
            repointCannotLinks(db, s->id, target_id);
            deleteCluster(db, s->id);
        }
        if (!updateCluster(db, merged))
            return false;
        // Persist the (possibly adopted) name on the surviving cluster and
        // its members.
        const bool named = DbRetry::withBusyRetry("FacesOps", "mergeClusters.name", [&]() {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            Poco::Nullable<int> pid;
            if (merged.person_id > 0)
                pid = merged.person_id;
            int tid = target_id;
            Poco::Data::Statement upd(sess);
            upd << "UPDATE face_clusters SET person_id = ? WHERE id = ?",
                use(pid), use(tid), now;
            Poco::Data::Statement updf(sess);
            updf << "UPDATE faces SET person_id = ? WHERE cluster_id = ?",
                use(pid), use(tid), now;
            return true;
        });
        if (!named)
            return false;
        // FACE-8: filter the combined membership — a merge is the moment
        // both looks' evidence is present, so misfits stand out sharpest.
        const int n = purgeMisfits(db, target_id, purge_cutoff);
        if (purged)
            *purged = n;
        return true;
    }

    int FacesOps::mergePersonClusters(DatabaseManager &db, int person_id,
                                      double purge_cutoff, int *purged)
    {
        if (purged)
            *purged = 0;
        if (person_id <= 0)
            return 0;
        // Largest cluster wins (ties: lowest id) — the accumulated evidence
        // is the identity; FACE-6 pin rules make a pinned cover survive
        // regardless of merge direction.
        int target = 0, target_size = -1;
        std::vector<int> sources;
        for (const auto &c : listClusters(db))
        {
            if (c.person_id != person_id)
                continue;
            if (c.size > target_size)
            {
                if (target > 0)
                    sources.push_back(target);
                target = c.id;
                target_size = c.size;
            }
            else
                sources.push_back(c.id);
        }
        if (target <= 0)
            return 0;
        if (sources.empty())
            return target;
        return mergeClusters(db, target, sources, purge_cutoff, purged) ? target : 0;
    }

    bool FacesOps::setClusterHidden(DatabaseManager &db, int cluster_id, bool hidden)
    {
        return DbRetry::withBusyRetry("FacesOps", "setClusterHidden", [&]() {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            int cid = cluster_id, h = hidden ? 1 : 0;
            Poco::Data::Statement stmt(sess);
            stmt << "UPDATE face_clusters SET hidden = ? WHERE id = ?", use(h), use(cid), now;
            return true;
        });
    }

    std::vector<int> FacesOps::listClusterFileIds(DatabaseManager &db, int cluster_id)
    {
        std::vector<int> out;
        try
        {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            int cid = cluster_id;
            Poco::Data::Statement stmt(sess);
            stmt << "SELECT DISTINCT file_id FROM faces WHERE cluster_id = ? ORDER BY file_id",
                use(cid), into(out), now;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("FacesOps").error("listClusterFileIds: %s", std::string(e.what()));
        }
        return out;
    }

    int FacesOps::personIdByName(DatabaseManager &db, const std::string &name)
    {
        if (name.empty())
            return 0;
        int id = 0;
        try
        {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            std::string n = name;
            Poco::Data::Statement stmt(sess);
            // Canonical names win over aliases when both exist.
            stmt << "SELECT id FROM persons WHERE name = ? COLLATE NOCASE LIMIT 1",
                use(n), into(id), now;
            if (id <= 0)
            {
                Poco::Data::Statement alias(sess);
                alias << "SELECT person_id FROM person_aliases WHERE alias = ? LIMIT 1",
                    use(n), into(id), now;
            }
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("FacesOps").error("personIdByName: %s", std::string(e.what()));
        }
        return id;
    }

    bool FacesOps::setPersonAliases(DatabaseManager &db, int person_id,
                                    const std::vector<std::string> &aliases)
    {
        return DbRetry::withBusyRetry("FacesOps", "setPersonAliases", [&]() {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            int pid = person_id;
            Poco::Data::Statement del(sess);
            del << "DELETE FROM person_aliases WHERE person_id = ?", use(pid), now;
            for (const auto &a : aliases)
            {
                if (a.empty())
                    continue;
                std::string alias = a;
                Poco::Data::Statement ins(sess);
                // OR IGNORE: an alias already claimed by another person is
                // skipped, never stolen.
                ins << "INSERT OR IGNORE INTO person_aliases (alias, person_id) VALUES (?, ?)",
                    use(alias), use(pid), now;
            }
            return true;
        });
    }

    std::vector<std::string> FacesOps::listPersonAliases(DatabaseManager &db, int person_id)
    {
        std::vector<std::string> out;
        try
        {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            int pid = person_id;
            Poco::Data::Statement stmt(sess);
            stmt << "SELECT alias FROM person_aliases WHERE person_id = ? ORDER BY alias",
                use(pid), into(out), now;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("FacesOps").error("listPersonAliases: %s", std::string(e.what()));
        }
        return out;
    }

    // ---- FACE-3b: per-face corrections -----------------------------------

    std::vector<FacesOps::ClusterFace> FacesOps::listClusterFaces(DatabaseManager &db,
                                                                  int cluster_id,
                                                                  int offset, int limit,
                                                                  int &total,
                                                                  double suspect_bar)
    {
        std::vector<ClusterFace> out;
        total = 0;
        try
        {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            int cid = cluster_id;
            {
                Poco::Data::Statement count(sess);
                count << "SELECT COUNT(*) FROM faces WHERE cluster_id = ?",
                    use(cid), into(total), now;
            }
            int off = offset, lim = limit;
            Poco::Data::Statement stmt(sess);
            stmt << "SELECT f.id, COALESCE(sf.file_path, ''), f.bbox_x, f.bbox_y, "
                    "f.bbox_w, f.bbox_h, f.quality, f.embedding "
                    "FROM faces f LEFT JOIN scanned_files sf ON sf.id = f.file_id "
                    "WHERE f.cluster_id = ? ORDER BY f.quality DESC, f.id LIMIT ? OFFSET ?",
                use(cid), use(lim), use(off);
            stmt.execute();
            Poco::Data::RecordSet rs(stmt);
            std::vector<std::vector<float>> embeddings;
            for (bool more = rs.moveFirst(); more; more = rs.moveNext())
            {
                ClusterFace f;
                f.face_id = rs[0].convert<int>();
                f.file_path = rs[1].convert<std::string>();
                f.x = rs[2].convert<double>();
                f.y = rs[3].convert<double>();
                f.w = rs[4].convert<double>();
                f.h = rs[5].convert<double>();
                f.quality = rs[6].convert<double>();
                embeddings.emplace_back(
                    rs[7].isEmpty() ? std::vector<float>{}
                                    : blobToFloats(rs[7].extract<Poco::Data::BLOB>()));
                out.push_back(std::move(f));
            }
            annotateFaceFits(db, cluster_id, out, embeddings, suspect_bar);
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("FacesOps").error("listClusterFaces: %s", std::string(e.what()));
        }
        return out;
    }

    void FacesOps::annotateFaceFits(DatabaseManager &db, int cluster_id,
                                    std::vector<ClusterFace> &faces,
                                    const std::vector<std::vector<float>> &embeddings,
                                    double suspect_bar)
    {
        // FACE-7d: fit = the face's score against its own cluster (centroid
        // plus exemplars, the exemplar set excluding the face itself — a
        // face must not vouch for itself). Low fit flags the face for
        // review in the popup; nothing is ever auto-removed.
        std::vector<float> centroid;
        int pinned_cover = 0, pinned = 0;
        for (const auto &c : listClusters(db))
            if (c.id == cluster_id)
            {
                centroid = c.centroid;
                pinned_cover = c.cover_face_id;
                pinned = c.cover_pinned ? 1 : 0;
                break;
            }
        std::vector<ExemplarRow> exemplars;
        for (auto &e : listClusterExemplars(db))
            if (e.cluster_id == cluster_id)
                exemplars.push_back(std::move(e));

        for (std::size_t i = 0; i < faces.size() && i < embeddings.size(); ++i)
        {
            if (embeddings[i].empty())
            {
                faces[i].fit = 1.0; // no embedding: nothing to judge it by
                continue;
            }
            double best = cosineF(embeddings[i], centroid);
            for (const auto &e : exemplars)
                if (e.face_id != faces[i].face_id)
                    best = std::max(best, cosineF(embeddings[i], e.embedding));
            faces[i].fit = best;
            // The pinned cover is the human-verified anchor — never suspect.
            faces[i].suspect = best < suspect_bar &&
                               !(pinned != 0 && faces[i].face_id == pinned_cover);
        }
    }

    int FacesOps::purgeMisfits(DatabaseManager &db, int cluster_id, double cutoff)
    {
        if (cutoff <= 0)
            return 0;
        // Judge against the CURRENT membership's model: refresh exemplars
        // first so a just-merged cluster scores faces with both looks'
        // exemplars, not the stale pre-merge set.
        refreshClusterExemplars(db, cluster_id);

        std::vector<float> centroid;
        int pinned_cover = 0;
        bool found = false;
        for (const auto &c : listClusters(db))
            if (c.id == cluster_id)
            {
                centroid = c.centroid;
                pinned_cover = c.cover_pinned ? c.cover_face_id : 0;
                found = true;
                break;
            }
        if (!found || centroid.empty())
            return 0;
        std::vector<ExemplarRow> exemplars;
        for (auto &e : listClusterExemplars(db))
            if (e.cluster_id == cluster_id)
                exemplars.push_back(std::move(e));

        std::vector<int> evict;
        try
        {
            for (const auto &m : loadMembers(db, cluster_id))
            {
                // The pinned cover is the human-verified anchor — exempt.
                if (m.id == pinned_cover)
                    continue;
                double best = cosineF(m.embedding, centroid);
                for (const auto &e : exemplars)
                    if (e.face_id != m.id) // a face must not vouch for itself
                        best = std::max(best, cosineF(m.embedding, e.embedding));
                if (best < cutoff)
                    evict.push_back(m.id);
            }
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("FacesOps").error("purgeMisfits: %s", std::string(e.what()));
            return 0;
        }
        if (evict.empty())
            return 0;

        const bool ok = DbRetry::withBusyRetry("FacesOps", "purgeMisfits", [&]() {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            for (int face_id : evict)
            {
                int fid = face_id;
                Poco::Data::Statement stmt(sess);
                stmt << "UPDATE faces SET cluster_id = NULL, person_id = NULL WHERE id = ?",
                    use(fid), now;
            }
            return true;
        });
        if (!ok)
            return 0;
        // Rebuild size/centroid/cover — and the exemplars again — from the
        // cleaned membership (recomputeCluster deletes an emptied cluster).
        recomputeCluster(db, cluster_id);
        Poco::Logger::get("FacesOps")
            .information("purgeMisfits: cluster %d shed %z face(s) below fit %.2f",
                         cluster_id, evict.size(), cutoff);
        return static_cast<int>(evict.size());
    }

    bool FacesOps::addCannotLink(DatabaseManager &db, int face_id, int cluster_id)
    {
        return DbRetry::withBusyRetry("FacesOps", "addCannotLink", [&]() {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            int fid = face_id, cid = cluster_id;
            Poco::Data::Statement stmt(sess);
            stmt << "INSERT OR IGNORE INTO face_cannot_link (face_id, cluster_id) VALUES (?, ?)",
                use(fid), use(cid), now;
            return true;
        });
    }

    std::vector<FacesOps::CannotLink> FacesOps::listCannotLinks(DatabaseManager &db)
    {
        std::vector<CannotLink> out;
        try
        {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            Poco::Data::Statement stmt(sess);
            stmt << "SELECT l.face_id, l.cluster_id, COALESCE(f.cluster_id, 0) "
                    "FROM face_cannot_link l LEFT JOIN faces f ON f.id = l.face_id";
            stmt.execute();
            Poco::Data::RecordSet rs(stmt);
            for (bool more = rs.moveFirst(); more; more = rs.moveNext())
                out.push_back({rs[0].convert<int>(), rs[1].convert<int>(), rs[2].convert<int>()});
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("FacesOps").error("listCannotLinks: %s", std::string(e.what()));
        }
        return out;
    }

    bool FacesOps::repointCannotLinks(DatabaseManager &db, int from, int to)
    {
        return DbRetry::withBusyRetry("FacesOps", "repointCannotLinks", [&]() {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            int f = from, t = to;
            // INSERT-then-DELETE instead of UPDATE: a face may already carry
            // a constraint against `to`, and UPDATE would collide on the PK.
            Poco::Data::Statement ins(sess);
            ins << "INSERT OR IGNORE INTO face_cannot_link (face_id, cluster_id) "
                   "SELECT face_id, ? FROM face_cannot_link WHERE cluster_id = ?",
                use(t), use(f), now;
            Poco::Data::Statement del(sess);
            del << "DELETE FROM face_cannot_link WHERE cluster_id = ?", use(f), now;
            return true;
        });
    }

    bool FacesOps::recomputeCluster(DatabaseManager &db, int cluster_id)
    {
        // Load remaining members' embeddings and rebuild the row in C++
        // (blobs are small; a cluster rarely exceeds a few thousand faces).
        struct Member
        {
            int id;
            double quality;
            std::vector<float> embedding;
        };
        std::vector<Member> members;
        try
        {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            int cid = cluster_id;
            Poco::Data::Statement stmt(sess);
            stmt << "SELECT id, quality, embedding FROM faces "
                    "WHERE cluster_id = ? AND embedding IS NOT NULL",
                use(cid);
            stmt.execute();
            Poco::Data::RecordSet rs(stmt);
            for (bool more = rs.moveFirst(); more; more = rs.moveNext())
            {
                Member m;
                m.id = rs[0].convert<int>();
                m.quality = rs[1].convert<double>();
                m.embedding = blobToFloats(rs[2].extract<Poco::Data::BLOB>());
                members.push_back(std::move(m));
            }
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("FacesOps").error("recomputeCluster: %s", std::string(e.what()));
            return false;
        }

        if (members.empty())
            return deleteCluster(db, cluster_id);

        // Preserve the person assignment and the FACE-6 pin state.
        int person_id = 0, pinned_cover = 0, pinned = 0;
        try
        {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            int cid = cluster_id;
            Poco::Data::Statement sel(sess);
            sel << "SELECT COALESCE(person_id, 0), COALESCE(cover_face_id, 0), cover_pinned "
                   "FROM face_clusters WHERE id = ?",
                use(cid), into(person_id), into(pinned_cover), into(pinned), now;
        }
        catch (...)
        {
        }

        std::vector<double> acc(members[0].embedding.size(), 0.0);
        const Member *cover = &members[0];
        const Member *pinned_member = nullptr;
        for (const auto &m : members)
        {
            if (m.embedding.size() == acc.size())
                for (std::size_t i = 0; i < acc.size(); ++i)
                    acc[i] += m.embedding[i];
            if (m.quality > cover->quality)
                cover = &m;
            if (pinned != 0 && m.id == pinned_cover)
                pinned_member = &m;
        }
        double norm = 0;
        for (double v : acc)
            norm += v * v;

        ClusterRow row;
        row.id = cluster_id;
        row.size = static_cast<int>(members.size());
        // FACE-6: a pinned cover stays as long as the face is still a
        // member; once it leaves, the pin clears and quality elects.
        if (pinned_member)
        {
            row.cover_face_id = pinned_member->id;
            row.cover_quality = pinned_member->quality;
            row.cover_pinned = true;
        }
        else
        {
            row.cover_face_id = cover->id;
            row.cover_quality = cover->quality;
        }
        row.centroid.resize(acc.size());
        const double inv = norm > 0 ? 1.0 / std::sqrt(norm) : 0.0;
        for (std::size_t i = 0; i < acc.size(); ++i)
            row.centroid[i] = static_cast<float>(acc[i] * inv);
        row.person_id = person_id;
        if (!updateCluster(db, row))
            return false;
        // FACE-7b: membership changed — re-select the exemplars.
        refreshClusterExemplars(db, cluster_id);
        return true;
    }

    bool FacesOps::removeFaceFromCluster(DatabaseManager &db, int face_id)
    {
        int cluster_id = 0;
        try
        {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            int fid = face_id;
            Poco::Data::Statement sel(sess);
            sel << "SELECT COALESCE(cluster_id, 0) FROM faces WHERE id = ?",
                use(fid), into(cluster_id), now;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("FacesOps").error("removeFaceFromCluster: %s", std::string(e.what()));
            return false;
        }
        if (cluster_id <= 0)
            return false;

        if (!addCannotLink(db, face_id, cluster_id))
            return false;
        const bool cleared = DbRetry::withBusyRetry("FacesOps", "removeFace.clear", [&]() {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            int fid = face_id;
            Poco::Data::Statement stmt(sess);
            stmt << "UPDATE faces SET cluster_id = NULL, person_id = NULL WHERE id = ?",
                use(fid), now;
            return true;
        });
        if (!cleared)
            return false;
        return recomputeCluster(db, cluster_id);
    }

    // ---- FACE-7b: exemplars ----------------------------------------------

    bool FacesOps::setClusterExemplars(DatabaseManager &db, int cluster_id,
                                       const std::vector<int> &face_ids)
    {
        return DbRetry::withBusyRetry("FacesOps", "setClusterExemplars", [&]() {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            int cid = cluster_id;
            Poco::Data::Statement del(sess);
            del << "DELETE FROM face_cluster_exemplars WHERE cluster_id = ?", use(cid), now;
            for (int fid : face_ids)
            {
                int f = fid;
                Poco::Data::Statement ins(sess);
                ins << "INSERT OR IGNORE INTO face_cluster_exemplars (cluster_id, face_id) "
                       "VALUES (?, ?)",
                    use(cid), use(f), now;
            }
            return true;
        });
    }

    std::vector<FacesOps::ExemplarRow> FacesOps::listClusterExemplars(DatabaseManager &db)
    {
        std::vector<ExemplarRow> out;
        try
        {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            Poco::Data::Statement stmt(sess);
            // A face that left its cluster (re-homed by the sweep) is not a
            // valid exemplar anymore — the membership join filters it out
            // until the owning cluster re-selects.
            stmt << "SELECT e.cluster_id, e.face_id, f.embedding "
                    "FROM face_cluster_exemplars e "
                    "JOIN faces f ON f.id = e.face_id AND f.cluster_id = e.cluster_id "
                    "WHERE f.embedding IS NOT NULL";
            stmt.execute();
            Poco::Data::RecordSet rs(stmt);
            for (bool more = rs.moveFirst(); more; more = rs.moveNext())
            {
                ExemplarRow r;
                r.cluster_id = rs[0].convert<int>();
                r.face_id = rs[1].convert<int>();
                r.embedding = blobToFloats(rs[2].extract<Poco::Data::BLOB>());
                out.push_back(std::move(r));
            }
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("FacesOps").error("listClusterExemplars: %s", std::string(e.what()));
        }
        return out;
    }

    std::vector<int> FacesOps::refreshClusterExemplars(DatabaseManager &db, int cluster_id,
                                                       int k, double spread,
                                                       double min_quality)
    {
        std::vector<MemberFace> members;
        int pinned_cover = 0, pinned = 0;
        try
        {
            members = loadMembers(db, cluster_id);
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            int cid = cluster_id;
            Poco::Data::Statement sel(sess);
            sel << "SELECT COALESCE(cover_face_id, 0), cover_pinned FROM face_clusters "
                   "WHERE id = ?",
                use(cid), into(pinned_cover), into(pinned), now;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("FacesOps").error("refreshClusterExemplars: %s",
                                                std::string(e.what()));
            return {};
        }

        // Quality-first farthest-point selection: the best face seeds the
        // set, later faces enter only when they represent a genuinely
        // different look (cosine < spread to every adopted exemplar). The
        // FACE-6 pinned cover is the human-verified anchor and always
        // participates, regardless of quality.
        std::stable_sort(members.begin(), members.end(),
                         [](const MemberFace &a, const MemberFace &b) {
                             return a.quality > b.quality;
                         });
        std::vector<int> chosen;
        std::vector<const std::vector<float> *> chosen_vecs;
        if (pinned != 0)
            for (const auto &m : members)
                if (m.id == pinned_cover)
                {
                    chosen.push_back(m.id);
                    chosen_vecs.push_back(&m.embedding);
                    break;
                }
        for (const auto &m : members)
        {
            if (static_cast<int>(chosen.size()) >= k)
                break;
            if (m.quality < min_quality)
                break; // quality-sorted: nothing below qualifies either
            if (std::find(chosen.begin(), chosen.end(), m.id) != chosen.end())
                continue;
            bool distinct = true;
            for (const auto *v : chosen_vecs)
                if (cosineF(m.embedding, *v) >= spread)
                {
                    distinct = false;
                    break;
                }
            if (!distinct)
                continue;
            chosen.push_back(m.id);
            chosen_vecs.push_back(&m.embedding);
        }
        if (!setClusterExemplars(db, cluster_id, chosen))
            return {};
        return chosen;
    }

    // ---- FACE-7e: dissolve ("Regroup faces") -----------------------------

    bool FacesOps::dissolveCluster(DatabaseManager &db, int cluster_id)
    {
        int person_id = 0;
        {
            try
            {
                auto lease = db.acquireSessionLease();
                Poco::Data::Session &sess = lease.get();
                int cid = cluster_id;
                int exists = 0;
                Poco::Data::Statement sel(sess);
                sel << "SELECT COUNT(*), COALESCE(MAX(person_id), 0) FROM face_clusters "
                       "WHERE id = ?",
                    use(cid), into(exists), into(person_id), now;
                if (exists <= 0)
                    return false;
            }
            catch (const std::exception &e)
            {
                Poco::Logger::get("FacesOps").error("dissolveCluster: %s",
                                                    std::string(e.what()));
                return false;
            }
        }

        const bool detached = DbRetry::withBusyRetry("FacesOps", "dissolveCluster", [&]() {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            int cid = cluster_id;
            // Detach, keep embeddings: the catch-up regroups these faces
            // under the current match rules — a wrongly fused pair of
            // people separates naturally. Deliberately NOT a file rescan
            // (that is kFaceScanVersion's job, and would wipe other
            // people's faces from shared photos).
            Poco::Data::Statement upd(sess);
            upd << "UPDATE faces SET cluster_id = NULL, person_id = NULL "
                   "WHERE cluster_id = ?",
                use(cid), now;
            // Corrections against a wrong grouping die with it.
            Poco::Data::Statement dell(sess);
            dell << "DELETE FROM face_cannot_link WHERE cluster_id = ?", use(cid), now;
            return true;
        });
        if (!detached || !deleteCluster(db, cluster_id)) // exemplars die too
            return false;

        if (person_id > 0)
        {
            // Orphan-clean the person + aliases when this was their only
            // cluster (the nameCluster rename-away pattern).
            return DbRetry::withBusyRetry("FacesOps", "dissolveCluster.person", [&]() {
                auto lease = db.acquireSessionLease();
                Poco::Data::Session &sess = lease.get();
                int pid = person_id;
                Poco::Data::Statement del(sess);
                del << "DELETE FROM persons WHERE id = ? AND NOT EXISTS "
                       "(SELECT 1 FROM face_clusters WHERE person_id = ?)",
                    use(pid), use(pid), now;
                Poco::Data::Statement dela(sess);
                dela << "DELETE FROM person_aliases WHERE person_id = ? AND NOT EXISTS "
                        "(SELECT 1 FROM persons WHERE id = ?)",
                    use(pid), use(pid), now;
                return true;
            });
        }
        return true;
    }

    // ---- FACE-6: pinned covers -------------------------------------------

    bool FacesOps::clearAllFaceGroups(DatabaseManager &db)
    {
        // FACE-8b: dissolveCluster, globally — one statement per table
        // instead of per-cluster loops. Faces keep their embeddings, so the
        // sweep's catch-up rebuilds fresh unnamed clusters within ticks;
        // cannot-links go too (they reference cluster ids that no longer
        // exist, and a factory reset means forgetting the corrections).
        return DbRetry::withBusyRetry("FacesOps", "clearAllFaceGroups", [&]() {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            for (const char *sql :
                 {"UPDATE faces SET cluster_id = NULL, person_id = NULL",
                  "DELETE FROM face_cluster_exemplars",
                  "DELETE FROM face_cannot_link",
                  "DELETE FROM face_clusters",
                  "DELETE FROM person_aliases",
                  "DELETE FROM persons"})
            {
                Poco::Data::Statement stmt(sess);
                stmt << std::string(sql), now;
            }
            return true;
        });
    }

    bool FacesOps::setClusterCover(DatabaseManager &db, int cluster_id, int face_id)
    {
        // Validate membership first — the popup may race the sweep
        // re-homing the face into another cluster.
        double quality = 0;
        int member = 0;
        try
        {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            int fid = face_id, cid = cluster_id;
            Poco::Data::Statement sel(sess);
            sel << "SELECT COUNT(*), COALESCE(MAX(quality), 0) FROM faces "
                   "WHERE id = ? AND cluster_id = ?",
                use(fid), use(cid), into(member), into(quality), now;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("FacesOps").error("setClusterCover: %s", std::string(e.what()));
            return false;
        }
        if (member <= 0)
            return false;

        return DbRetry::withBusyRetry("FacesOps", "setClusterCover", [&]() {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            int fid = face_id, cid = cluster_id;
            double q = quality;
            // cover_quality stays accurate for merge comparisons.
            Poco::Data::Statement stmt(sess);
            stmt << "UPDATE face_clusters SET cover_face_id = ?, cover_quality = ?, "
                    "cover_pinned = 1 WHERE id = ?",
                use(fid), use(q), use(cid), now;
            return true;
        });
    }
}
