#include "database/faces_ops.hpp"

#include "database/database_manager.hpp"
#include "database/db_retry.hpp"

#include <Poco/Data/RecordSet.h>
#include <Poco/Data/Session.h>
#include <Poco/Data/Statement.h>
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

        // scanned_files migrations (each ALTER adds one column; a failure
        // means the column already exists).
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
              "person_id INTEGER NOT NULL) WITHOUT ROWID"})
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

    std::vector<FacesOps::ClusterRow> FacesOps::listClusters(DatabaseManager &db)
    {
        std::vector<ClusterRow> out;
        try
        {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            Poco::Data::Statement stmt(sess);
            stmt << "SELECT id, centroid, size, COALESCE(cover_face_id, 0), cover_quality, "
                    "COALESCE(person_id, 0) FROM face_clusters ORDER BY id";
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
            int size = row.size, cover = row.cover_face_id;
            double cq = row.cover_quality;
            Poco::Data::Statement stmt(sess);
            stmt << "INSERT INTO face_clusters (centroid, size, cover_face_id, cover_quality) "
                    "VALUES (?, ?, ?, ?)",
                use(blob), use(size), use(cover), use(cq), now;
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
            int id = row.id, size = row.size, cover = row.cover_face_id;
            double cq = row.cover_quality;
            Poco::Data::Statement stmt(sess);
            stmt << "UPDATE face_clusters SET centroid = ?, size = ?, cover_face_id = ?, "
                    "cover_quality = ? WHERE id = ?",
                use(blob), use(size), use(cover), use(cq), use(id), now;
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
                "  COALESCE(c.person_id, 0) "
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
                                 const std::vector<int> &source_ids)
    {
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
            if (s->cover_quality > merged.cover_quality)
            {
                merged.cover_quality = s->cover_quality;
                merged.cover_face_id = s->cover_face_id;
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
            deleteCluster(db, s->id);
        }
        if (!updateCluster(db, merged))
            return false;
        // Persist the (possibly adopted) name on the surviving cluster and
        // its members.
        return DbRetry::withBusyRetry("FacesOps", "mergeClusters.name", [&]() {
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
                                                                  int &total)
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
                    "f.bbox_w, f.bbox_h, f.quality "
                    "FROM faces f LEFT JOIN scanned_files sf ON sf.id = f.file_id "
                    "WHERE f.cluster_id = ? ORDER BY f.quality DESC, f.id LIMIT ? OFFSET ?",
                use(cid), use(lim), use(off);
            stmt.execute();
            Poco::Data::RecordSet rs(stmt);
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
                out.push_back(std::move(f));
            }
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("FacesOps").error("listClusterFaces: %s", std::string(e.what()));
        }
        return out;
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

        std::vector<double> acc(members[0].embedding.size(), 0.0);
        const Member *cover = &members[0];
        for (const auto &m : members)
        {
            if (m.embedding.size() == acc.size())
                for (std::size_t i = 0; i < acc.size(); ++i)
                    acc[i] += m.embedding[i];
            if (m.quality > cover->quality)
                cover = &m;
        }
        double norm = 0;
        for (double v : acc)
            norm += v * v;

        ClusterRow row;
        row.id = cluster_id;
        row.size = static_cast<int>(members.size());
        row.cover_face_id = cover->id;
        row.cover_quality = cover->quality;
        row.centroid.resize(acc.size());
        const double inv = norm > 0 ? 1.0 / std::sqrt(norm) : 0.0;
        for (std::size_t i = 0; i < acc.size(); ++i)
            row.centroid[i] = static_cast<float>(acc[i] * inv);
        // Preserve the person assignment.
        int person_id = 0;
        try
        {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            int cid = cluster_id;
            Poco::Data::Statement sel(sess);
            sel << "SELECT COALESCE(person_id, 0) FROM face_clusters WHERE id = ?",
                use(cid), into(person_id), now;
        }
        catch (...)
        {
        }
        row.person_id = person_id;
        return updateCluster(db, row);
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
}
