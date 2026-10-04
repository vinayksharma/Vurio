#include "database/scanned_files_ops.hpp"
#include "config/config_enums.hpp"
#include "database/database_manager.hpp"
#include "database/db_retry.hpp"
#include "database/faces_ops.hpp"
#include "database/places_ops.hpp"
#include "database/vector_index.hpp"
#include "database/sql_constants.hpp"
#include "media_processors/image/backends/exif_reader.hpp"
#include <Poco/Data/Session.h>
#include <Poco/Data/Statement.h>
#include <Poco/Data/RecordSet.h>
#include <Poco/Nullable.h>
#include <cmath>

namespace MediaDedup
{
    using namespace Poco::Data;

    namespace
    {
        // DF-4: edges were removed for deleted file(s) inside `sess`'s open
        // transaction; ask the finder to re-cluster on its next run (a removed
        // edge can split a component). Persisted flag because this code has
        // no handle on the DuplicateFinder. Best-effort: on a database where
        // the finder has never initialised, duplicate_edges_meta (or the
        // column) does not exist and there are no groups to go stale.
        // The edge table is created by DuplicateGroupsOps::ensureTables; a
        // database where the finder has never run (or a unit-test fixture)
        // may not have it, and a delete must not fail because of that.
        bool duplicateEdgesTableExists(Session &sess)
        {
            int n = 0;
            sess << "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='duplicate_edges'",
                Keywords::into(n), Keywords::now;
            return n > 0;
        }

        // DF-4: remove every edge touching `file_id` (either column). Returns
        // the number removed; 0 when the edge table does not exist.
        int deleteEdgesForFile(Session &sess, int file_id)
        {
            if (!duplicateEdgesTableExists(sess))
                return 0;
            Statement del_edges(sess);
            int id_a = file_id, id_b = file_id;
            del_edges << std::string(SQL::kDeleteDuplicateEdgesForFile), Keywords::use(id_a), Keywords::use(id_b);
            del_edges.execute();
            int purged = 0;
            sess << "SELECT changes()", Keywords::into(purged), Keywords::now;
            return purged;
        }

        void requestRegroupAfterEdgePurge(Session &sess, int purged_edges, const std::string &what)
        {
            try
            {
                sess << std::string(SQL::kRequestRegroupAfterEdgePurge), Keywords::now;
                Poco::Logger::get("ScannedFilesOps").information(
                    "Removed %d duplicate edge(s) for %s; re-cluster requested", purged_edges, what);
            }
            catch (const std::exception &e)
            {
                Poco::Logger::get("ScannedFilesOps").debug(
                    "Removed %d duplicate edge(s) for %s; could not flag re-cluster (%s)",
                    purged_edges, what, std::string(e.what()));
            }
        }
    }

    bool ScannedFilesOps::ensureTable(DatabaseManager &db)
    {
        // Create table first
        if (!db.ensureTableExists("scanned_files", SQL::kCreateScannedFilesTable))
        {
            return false;
        }

        // ADJ-1: image_adjustments is a dependent of scanned_files and the
        // list queries' best-thumbnail subquery correlates on it, so it must
        // exist wherever scanned_files does (tests included). Created here,
        // without linking the adjustments module, from the same constants
        // AdjustmentsOps::ensureTable uses.
        if (!db.ensureTableExists("image_adjustments", SQL::kCreateImageAdjustmentsTable))
        {
            return false;
        }
        try
        {
            auto lease = db.acquireSessionLease();
            lease.get() << std::string(SQL::kCreateImageAdjustmentsIndexLocation), Keywords::now;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ScannedFilesOps").warning("image_adjustments index: %s", std::string(e.what()));
        }

        // Ensure ignore_reason column exists (migration)
        try {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            Poco::Data::Statement stmt(sess);
            stmt << "ALTER TABLE scanned_files ADD COLUMN ignore_reason TEXT", Poco::Data::Keywords::now;
            Poco::Logger::get("ScannedFilesOps").information("Added ignore_reason column to scanned_files table.");
        } catch (const std::exception &) {
            // Column likely already exists, ignore
        }

        // Ensure the generated taken_day column exists (migration). VIRTUAL, so
        // the ALTER is instant; the partial index below materializes its values
        // and makes the day-view queries index-served.
        try {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            Poco::Data::Statement stmt(sess);
            stmt << std::string(SQL::kAlterScannedFilesAddTakenDay), Poco::Data::Keywords::now;
            Poco::Logger::get("ScannedFilesOps").information("Added taken_day generated column to scanned_files table.");
        } catch (const std::exception &) {
            // Column likely already exists, ignore
        }

        // DB-1: ensure the generated is_video column exists (migration; VIRTUAL,
        // instant). The day-view filter and the taken_day partial index depend
        // on it.
        try {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            Poco::Data::Statement stmt(sess);
            stmt << std::string(SQL::kAlterScannedFilesAddIsVideo), Poco::Data::Keywords::now;
            Poco::Logger::get("ScannedFilesOps").information("Added is_video generated column to scanned_files table.");
        } catch (const std::exception &) {
            // Column likely already exists, ignore
        }

        // EXIF-2: ensure the comprehensive EXIF columns exist (migration for
        // pre-existing databases; fresh installs get them from the CREATE
        // TABLE). Each ALTER fails harmlessly if the column already exists.
        {
            static constexpr std::string_view kExifMigrations[] = {
                SQL::kAlterScannedFilesAddExifMake,
                SQL::kAlterScannedFilesAddExifModel,
                SQL::kAlterScannedFilesAddExifLensModel,
                SQL::kAlterScannedFilesAddExifISO,
                SQL::kAlterScannedFilesAddExifFNumber,
                SQL::kAlterScannedFilesAddExifShutterSpeedAPEX,
                SQL::kAlterScannedFilesAddExifShutterSpeedText,
                SQL::kAlterScannedFilesAddExifExposureTime,
                SQL::kAlterScannedFilesAddExifExposureComp,
                SQL::kAlterScannedFilesAddExifMeteringMode,
                SQL::kAlterScannedFilesAddExifFlashFired,
                SQL::kAlterScannedFilesAddExifFlashMode,
                SQL::kAlterScannedFilesAddExifFocalLength,
                SQL::kAlterScannedFilesAddExifFocalLength35mm,
                SQL::kAlterScannedFilesAddExifFocusDistance,
                SQL::kAlterScannedFilesAddExifFocusMode,
                SQL::kAlterScannedFilesAddExifWhiteBalance,
                SQL::kAlterScannedFilesAddExifColorSpace,
                SQL::kAlterScannedFilesAddExifSceneMode,
                SQL::kAlterScannedFilesAddExifBrightness,
                SQL::kAlterScannedFilesAddExifContrast,
                SQL::kAlterScannedFilesAddExifSaturation,
                SQL::kAlterScannedFilesAddExifSharpness,
                SQL::kAlterScannedFilesAddExifGPSLat,
                SQL::kAlterScannedFilesAddExifGPSLon,
                SQL::kAlterScannedFilesAddExifGPSAlt,
                SQL::kAlterScannedFilesAddExifOrientation,
                SQL::kAlterScannedFilesAddPlaceLabel,
                SQL::kAlterScannedFilesAddPlaceLevel,
                SQL::kAlterScannedFilesAddShoreDistance,
            };
            int added = 0;
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            for (const auto &migration : kExifMigrations)
            {
                try {
                    sess << std::string(migration), Poco::Data::Keywords::now;
                    ++added;
                } catch (const std::exception &) {
                    // Column already exists, ignore
                }
            }
            if (added > 0)
                Poco::Logger::get("ScannedFilesOps").information(
                    "EXIF-2: added %d EXIF column(s) to scanned_files table.", added);
        }

        // DB-1: one-time index upgrade — an older idx_scanned_files_taken_day
        // without the is_video predicate cannot exclude video rows index-side,
        // and the new day-view filter (`is_video = 0`) would not match it.
        // Detect by definition text and drop; the CREATE below rebuilds it.
        try {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            std::string index_sql;
            Poco::Data::Statement query(sess);
            query << "SELECT COALESCE(sql,'') FROM sqlite_master WHERE type='index' AND name='idx_scanned_files_taken_day'",
                Poco::Data::Keywords::into(index_sql), Poco::Data::Keywords::now;
            if (!index_sql.empty() && index_sql.find("is_video") == std::string::npos)
            {
                Poco::Data::Statement drop(sess);
                drop << "DROP INDEX idx_scanned_files_taken_day", Poco::Data::Keywords::now;
                Poco::Logger::get("ScannedFilesOps").information(
                    "Dropped pre-is_video taken_day index; rebuilding with the video-excluding predicate (one-time migration).");
            }
        } catch (const std::exception &e) {
            // No such index / no row — the CREATE below handles creation.
            Poco::Logger::get("ScannedFilesOps").debug("taken_day index upgrade check skipped: " + std::string(e.what()));
        }

        // FILT-1: repair rows the resolution filter ignored and the caller then
        // un-ignored. Cheap and idempotent — after the first run it matches
        // nothing — so it needs no version marker of its own.
        try {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            sess << std::string(SQL::kRepairUnIgnoredFiles), Poco::Data::Keywords::now;
            int repaired = 0;
            Poco::Data::Statement cnt(sess);
            cnt << "SELECT changes()", Poco::Data::Keywords::into(repaired), Poco::Data::Keywords::now;
            if (repaired > 0) {
                Poco::Logger::get("ScannedFilesOps").information(
                    "FILT-1: restored ignored state on %d file(s) that had been marked processed despite being filtered out.",
                    repaired);
            }
        } catch (const std::exception &e) {
            Poco::Logger::get("ScannedFilesOps").warning(
                "FILT-1 repair failed: " + std::string(e.what()));
        }

        // Create indexes for efficient lookups (critical for FilesManager performance)
        // Note: UNIQUE constraint on file_path already creates an implicit index, but we add explicit one for clarity
        try
        {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();

            // One fresh Statement per index (a reused named Statement
            // accumulates SQL across executes and silently drops later
            // statements — this bit DB-6's index until switched to the
            // `sess <<` per-statement pattern the other ops files use).

            // Create file_path index
            sess << std::string(SQL::kCreateScannedFilesIndexFilePath), Poco::Data::Keywords::now;
            Poco::Logger::get("ScannedFilesOps").debug("Created index on scanned_files.file_path");

            // Create composite index for filtered queries
            sess << std::string(SQL::kCreateScannedFilesIndexLocationProcessed), Poco::Data::Keywords::now;
            Poco::Logger::get("ScannedFilesOps").debug("Created composite index for location_key filtering");

            // Partial index on the generated taken_day column (day-view queries)
            sess << std::string(SQL::kCreateScannedFilesIndexTakenDay), Poco::Data::Keywords::now;
            Poco::Logger::get("ScannedFilesOps").debug("Created taken_day index for day-view queries");

            // DB-6: partial index for the unprocessed sweep (every tick)
            sess << std::string(SQL::kCreateScannedFilesIndexUnprocessed), Poco::Data::Keywords::now;
            Poco::Logger::get("ScannedFilesOps").debug("Created unprocessed partial index for the media sweep");

            // EXIF-2: indices for searchable EXIF fields
            sess << std::string(SQL::kCreateScannedFilesIndexExifISO), Poco::Data::Keywords::now;
            sess << std::string(SQL::kCreateScannedFilesIndexExifFNumber), Poco::Data::Keywords::now;
            sess << std::string(SQL::kCreateScannedFilesIndexExifExposureComp), Poco::Data::Keywords::now;
            sess << std::string(SQL::kCreateScannedFilesIndexExifMakeModel), Poco::Data::Keywords::now;
            sess << std::string(SQL::kCreateScannedFilesIndexExifWhiteBalance), Poco::Data::Keywords::now;
            sess << std::string(SQL::kCreateScannedFilesIndexExifDateTime), Poco::Data::Keywords::now;
            sess << std::string(SQL::kCreateScannedFilesIndexExifGPS), Poco::Data::Keywords::now;
            Poco::Logger::get("ScannedFilesOps").debug("Created EXIF search indices");
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ScannedFilesOps").warning("Failed to create indexes (may already exist): " + std::string(e.what()));
            // Don't fail - table exists, indexes are just optimizations
        }

        return true;
    }

    static void bindUpsertParams(Statement &stmt, const ScannedFileRow &r)
    {
        std::string file_path = r.file_path;
        std::string relative_path = r.relative_path;
        std::string share_name = r.share_name;
        std::string file_name = r.file_name;
        std::string file_metadata = r.file_metadata;
        int p = r.processed; // Preserve state code (0,1,2,...)
        std::string l = r.links;
        int inf = r.is_network_file ? 1 : 0;
        std::string location_key = r.location_key;
        std::string exif_dt = r.exif_datetime.value_or("");
        std::string exif_cam = r.exif_camera.value_or("");
        std::string exif_res = r.exif_resolution.value_or("");
        std::string ignore_reason = r.ignore_reason.value_or("");

        stmt << std::string(SQL::kUpsertScannedFile),
            Keywords::use(file_path),
            Keywords::use(relative_path),
            Keywords::use(share_name),
            Keywords::use(file_name),
            Keywords::use(file_metadata),
            Keywords::use(p),
            Keywords::use(l),
            Keywords::use(inf),
            Keywords::use(location_key),
            Keywords::use(exif_dt),
            Keywords::use(exif_cam),
            Keywords::use(exif_res),
            Keywords::use(ignore_reason),
            Keywords::now;
    }

    bool ScannedFilesOps::upsert(DatabaseManager &db, const ScannedFileRow &row)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            bindUpsertParams(stmt, row);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    int ScannedFilesOps::upsertBatch(DatabaseManager &db, const std::vector<ScannedFileRow> &rows)
    {
        if (rows.empty())
            return 0;
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            try
            {
                sess.begin();
                for (const auto &row : rows)
                {
                    Statement stmt(sess);
                    bindUpsertParams(stmt, row);
                }
                sess.commit();
            }
            catch (...)
            {
                try { sess.rollback(); } catch (...) {}
                throw;
            }
            return static_cast<int>(rows.size());
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ScannedFilesOps").error("upsertBatch failed (%zu rows): %s", rows.size(), std::string(e.what()));
            return 0;
        }
    }

    bool ScannedFilesOps::removeByPath(DatabaseManager &db, const std::string &file_path)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            std::string p = file_path;

            // DF-2: capture the id first so the KNN index row can be removed.
            int file_id = 0;
            {
                Statement sel(sess);
                sel << std::string(SQL::kSelectScannedFileIdByPath), Keywords::into(file_id), Keywords::use(p);
                sel.execute();
            }

            // DF-4: the file's similarity edges go in the same transaction as
            // the row (edges have no FK, so nothing cascades them).
            std::vector<int> touched_clusters;
            try
            {
                sess.begin();
                if (file_id > 0)
                {
                    const int purged = deleteEdgesForFile(sess, file_id);
                    if (purged > 0)
                        requestRegroupAfterEdgePurge(sess, purged, file_path);
                }
                // FACE-9: the row's faces go with it, in the same
                // transaction. Nothing else deletes them, and a face that
                // outlives its photo cannot be shown, counted or clustered.
                if (file_id > 0)
                {
                    touched_clusters = FacesOps::deleteFacesForFiles(sess, {file_id});
                    // PLACE-1: same for its place rows — an orphan would keep
                    // its place "with photos" for name search.
                    PlacesOps::deleteFilePlacesForFiles(sess, {file_id});
                }
                Statement stmt(sess);
                stmt << std::string(SQL::kDeleteScannedFile), Keywords::use(p), Keywords::now;
                sess.commit();
            }
            catch (...)
            {
                try { sess.rollback(); } catch (...) {}
                throw;
            }

            // Rebuild what the departed faces were counted in, after commit
            // (each recompute takes its own session).
            FacesOps::repairClustersAfterFaceDeletion(db, touched_clusters);

            if (file_id > 0)
            {
                if (VectorIndex *vix = db.getVectorIndex())
                    vix->remove(file_id);
            }
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool ScannedFilesOps::removeByLocationKey(DatabaseManager &db, const std::string &location_key)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            std::string key = location_key;

            // DB-7: collect the cache-file paths FIRST (plain reads). The
            // unlinks run AFTER commit — previously thousands of filesystem
            // removes executed inside the write transaction, holding the
            // single SQLite writer lock across syscalls.
            std::vector<std::string> cache_files;
            std::vector<int> removed_ids; // DF-2: for vector-index cleanup
            std::vector<int> touched_clusters; // FACE-9: clusters to rebuild
            {
                Statement sel_thumbs(sess);
                sel_thumbs << "SELECT cached_path FROM thumbnail_cache WHERE source_path IN (SELECT file_path FROM scanned_files WHERE location_key=?)",
                    Keywords::into(cache_files), Keywords::use(key), Keywords::now;
                Statement sel_previews(sess);
                sel_previews << "SELECT cached_path FROM preview_cache WHERE source_path IN (SELECT file_path FROM scanned_files WHERE location_key=?)",
                    Keywords::into(cache_files), Keywords::use(key), Keywords::now;
                Statement sel_ids(sess);
                sel_ids << "SELECT id FROM scanned_files WHERE location_key=?",
                    Keywords::into(removed_ids), Keywords::use(key), Keywords::now;
            }

            try
            {
            sess.begin();

            // Delete from dependent tables using subqueries
            Statement del_members(sess);
            del_members << "DELETE FROM duplicate_members WHERE file_id IN (SELECT id FROM scanned_files WHERE location_key=?)", Keywords::use(key), Keywords::now;

            Statement del_groups(sess);
            del_groups << "DELETE FROM duplicate_groups WHERE representative_file_path IN (SELECT file_path FROM scanned_files WHERE location_key=?)", Keywords::use(key), Keywords::now;
            
            // Delete orphaned members of those groups that just got deleted
            Statement del_orphaned_members(sess);
            del_orphaned_members << "DELETE FROM duplicate_members WHERE group_id NOT IN (SELECT id FROM duplicate_groups)", Keywords::now;
            
            // Update member counts for remaining groups
            Statement update_counts(sess);
            update_counts << "UPDATE duplicate_groups SET member_count = (SELECT COUNT(*) FROM duplicate_members WHERE group_id = duplicate_groups.id)", Keywords::now;

            // Delete groups with less than 2 members
            Statement del_small_groups(sess);
            del_small_groups << "DELETE FROM duplicate_groups WHERE member_count < 2", Keywords::now;
            
            Statement del_small_groups_members(sess);
            del_small_groups_members << "DELETE FROM duplicate_members WHERE group_id NOT IN (SELECT id FROM duplicate_groups)", Keywords::now;

            Statement del_artifacts(sess);
            del_artifacts << "DELETE FROM image_artifacts WHERE file_path IN (SELECT file_path FROM scanned_files WHERE location_key=?)", Keywords::use(key), Keywords::now;

            Statement del_errors(sess);
            del_errors << "DELETE FROM processing_errors WHERE file_path IN (SELECT file_path FROM scanned_files WHERE location_key=?)", Keywords::use(key), Keywords::now;

            Statement del_thumbs(sess);
            del_thumbs << "DELETE FROM thumbnail_cache WHERE source_path IN (SELECT file_path FROM scanned_files WHERE location_key=?)", Keywords::use(key), Keywords::now;

            Statement del_previews(sess);
            del_previews << "DELETE FROM preview_cache WHERE source_path IN (SELECT file_path FROM scanned_files WHERE location_key=?)", Keywords::use(key), Keywords::now;

            // ADJ-1: the location's adjustment rows go with it (the sidecar
            // files on disk are handled before this by the deregistration
            // flow, per the user's keep/delete choice).
            Statement del_adjustments(sess);
            del_adjustments << std::string(SQL::kDeleteImageAdjustmentsByLocation), Keywords::use(key), Keywords::now;

            Statement del_ejected(sess);
            del_ejected << "DELETE FROM ejected_files_history WHERE original_location_key=?", Keywords::use(key), Keywords::now;

            // DF-4: take the location's similarity edges with it, in the same
            // transaction, BEFORE the scanned_files rows go (the subqueries
            // read them). Index-driven on both columns.
            if (duplicateEdgesTableExists(sess))
            {
                Statement del_edges_a(sess);
                del_edges_a << std::string(SQL::kDeleteDuplicateEdgesForLocationA), Keywords::use(key), Keywords::now;
                int purged_a = 0;
                sess << "SELECT changes()", Keywords::into(purged_a), Keywords::now;
                Statement del_edges_b(sess);
                del_edges_b << std::string(SQL::kDeleteDuplicateEdgesForLocationB), Keywords::use(key), Keywords::now;
                int purged_b = 0;
                sess << "SELECT changes()", Keywords::into(purged_b), Keywords::now;
                if (purged_a + purged_b > 0)
                    requestRegroupAfterEdgePurge(sess, purged_a + purged_b, "location " + key);
            }

            // FACE-9: the location's faces go with its rows, same
            // transaction (see removeByPath).
            touched_clusters = FacesOps::deleteFacesForFiles(sess, removed_ids);
            // PLACE-1: and their place rows (see removeByPath).
            PlacesOps::deleteFilePlacesForFiles(sess, removed_ids);

            Statement del_files(sess);
            del_files << "DELETE FROM scanned_files WHERE location_key=?", Keywords::use(key), Keywords::now;

            sess.commit();
            }
            catch (...)
            {
                // DB-7: rollback on the SAME session — the old code acquired a
                // NEW lease and rolled that one back (a no-op) while the real
                // transaction unwound with the pool return.
                try { sess.rollback(); } catch (...) {}
                throw;
            }

            // Rebuild the clusters the departed faces were counted in,
            // after commit (each recompute takes its own session). A cluster
            // whose every photo lived here is deleted.
            FacesOps::repairClustersAfterFaceDeletion(db, touched_clusters);

            // Unlink cached assets AFTER the commit, outside any transaction.
            int deleted_files = 0;
            for (const auto &cached_path : cache_files)
            {
                try
                {
                    if (std::filesystem::exists(cached_path))
                    {
                        std::filesystem::remove(cached_path);
                        deleted_files++;
                    }
                }
                catch (...) {}
            }
            if (deleted_files > 0)
                Poco::Logger::get("ScannedFilesOps").information("Deleted %d cached media files for location key: %s", deleted_files, location_key);

            // DF-2: purge the deleted files' vectors from the KNN index.
            if (!removed_ids.empty())
            {
                if (VectorIndex *vix = db.getVectorIndex())
                    vix->removeMany(removed_ids);
            }

            Poco::Logger::get("ScannedFilesOps").information("Successfully removed all references for location key: " + location_key);
            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ScannedFilesOps").error("Failed to remove by location key: %s", std::string(e.what()));
            return false;
        }
    }

    bool ScannedFilesOps::completelyRemoveById(DatabaseManager &db, int file_id)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();

            // DB-7: plain reads first (path + cached assets); unlinks happen
            // after commit so the write transaction is pure DB work.
            std::string file_path;
            {
                Statement sel_path(sess);
                sel_path << "SELECT file_path FROM scanned_files WHERE id=?", Keywords::into(file_path), Keywords::use(file_id);
                if (sel_path.execute() == 0 || file_path.empty())
                    return false; // File doesn't exist
            }

            std::vector<std::string> cache_files;
            std::vector<int> touched_clusters; // FACE-9: clusters to rebuild
            {
                Statement sel_thumbs(sess);
                sel_thumbs << "SELECT cached_path FROM thumbnail_cache WHERE source_path=?",
                    Keywords::into(cache_files), Keywords::use(file_path), Keywords::now;
                Statement sel_previews(sess);
                sel_previews << "SELECT cached_path FROM preview_cache WHERE source_path=?",
                    Keywords::into(cache_files), Keywords::use(file_path), Keywords::now;
            }

            try
            {
                sess.begin();

                Statement del_artifacts(sess);
                del_artifacts << "DELETE FROM image_artifacts WHERE file_path=?", Keywords::use(file_path);
                del_artifacts.execute();

                Statement del_errors(sess);
                del_errors << "DELETE FROM processing_errors WHERE file_path=?", Keywords::use(file_path);
                del_errors.execute();

                Statement del_thumbs(sess);
                del_thumbs << "DELETE FROM thumbnail_cache WHERE source_path=?", Keywords::use(file_path);
                del_thumbs.execute();

                Statement del_previews(sess);
                del_previews << "DELETE FROM preview_cache WHERE source_path=?", Keywords::use(file_path);
                del_previews.execute();

                // ADJ-1: a purged file's adjustments go with it. (removeByPath
                // deliberately keeps them: the scanner re-inserts a changed
                // file under the same path and the edits must survive.)
                Statement del_adjustments(sess);
                del_adjustments << std::string(SQL::kDeleteImageAdjustmentByPath), Keywords::use(file_path);
                del_adjustments.execute();

                // DF-4: the file's similarity edges go in the same transaction
                // (edges are canonical file_a < file_b, so check both columns;
                // PK + idx_duplicate_edges_file_b make this two index seeks).
                {
                    const int purged = deleteEdgesForFile(sess, file_id);
                    if (purged > 0)
                        requestRegroupAfterEdgePurge(sess, purged, file_path);
                }

                // FACE-9: the file's faces go with the row, same
                // transaction (see removeByPath).
                touched_clusters = FacesOps::deleteFacesForFiles(sess, {file_id});
                // PLACE-1: and its place rows (see removeByPath).
                PlacesOps::deleteFilePlacesForFiles(sess, {file_id});

                // Delete the scanned_file row itself. This will CASCADE DELETE
                // to: duplicate_members, ejected_files_history.
                Statement del_file(sess);
                del_file << "DELETE FROM scanned_files WHERE id=?", Keywords::use(file_id);
                del_file.execute();

                sess.commit();
            }
            catch (...)
            {
                // Rollback on the SAME session (see removeByLocationKey).
                try { sess.rollback(); } catch (...) {}
                throw;
            }

            // Rebuild what the departed faces were counted in, after commit
            // (each recompute takes its own session).
            FacesOps::repairClustersAfterFaceDeletion(db, touched_clusters);

            for (const auto &cached_path : cache_files)
            {
                try {
                    if (std::filesystem::exists(cached_path)) {
                        std::filesystem::remove(cached_path);
                    }
                } catch (...) {}
            }

            // DF-2: keep the KNN index honest — stale vectors for deleted
            // files poisoned every neighbor search (remove() had no callers).
            if (VectorIndex *vix = db.getVectorIndex())
                vix->remove(file_id);
            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ScannedFilesOps").error("Failed to completely remove by id %d: %s", file_id, std::string(e.what()));
            return false;
        }
    }

    std::optional<ScannedFileRow> ScannedFilesOps::getByPath(DatabaseManager &db, const std::string &file_path)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            std::string p = file_path;
            stmt << std::string(SQL::kSelectScannedFileByPath), Keywords::use(p), Keywords::now;
            RecordSet rs(stmt);
            if (!rs.moveFirst())
                return std::nullopt;
            ScannedFileRow r;
            r.id = rs[0].convert<int>();
            r.file_path = rs[1].convert<std::string>();
            r.relative_path = rs[2].convert<std::string>();
            r.share_name = rs[3].convert<std::string>();
            r.file_name = rs[4].convert<std::string>();
            r.file_metadata = rs[5].convert<std::string>();
            r.processed = rs[6].convert<int>();
            r.links = rs[7].convert<std::string>();
            r.is_network_file = rs[8].convert<int>() != 0;
            r.location_key = rs[9].convert<std::string>();
            r.created_at = rs[10].convert<std::string>();
            if (!rs[11].isEmpty()) r.exif_datetime = rs[11].convert<std::string>();
            if (!rs[12].isEmpty()) r.exif_camera = rs[12].convert<std::string>();
            if (!rs[13].isEmpty()) r.exif_resolution = rs[13].convert<std::string>();
                if (rs.columnCount() > 14 && !rs[14].isEmpty()) r.ignore_reason = rs[14].convert<std::string>();
            // Note: exif_blob is intentionally excluded from this SELECT query to save memory
            return r;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    std::vector<ScannedFileRow> ScannedFilesOps::listAll(DatabaseManager &db)
    {
        std::vector<ScannedFileRow> out;
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            stmt << std::string(SQL::kListScannedFiles), Keywords::now;
            RecordSet rs(stmt);
            bool more = rs.moveFirst();
            while (more)
            {
                ScannedFileRow r;
                r.id = rs[0].convert<int>();
                r.file_path = rs[1].convert<std::string>();
                r.relative_path = rs[2].convert<std::string>();
                r.share_name = rs[3].convert<std::string>();
                r.file_name = rs[4].convert<std::string>();
                r.file_metadata = rs[5].convert<std::string>();
                r.processed = rs[6].convert<int>();
                r.links = rs[7].convert<std::string>();
                r.is_network_file = rs[8].convert<int>() != 0;
                r.location_key = rs[9].convert<std::string>();
                r.created_at = rs[10].convert<std::string>();
                if (!rs[11].isEmpty()) r.exif_datetime = rs[11].convert<std::string>();
                if (!rs[12].isEmpty()) r.exif_camera = rs[12].convert<std::string>();
                if (!rs[13].isEmpty()) r.exif_resolution = rs[13].convert<std::string>();
                if (rs.columnCount() > 14 && !rs[14].isEmpty()) r.ignore_reason = rs[14].convert<std::string>();
                out.emplace_back(std::move(r));
                more = rs.moveNext();
            }
        }
        catch (...)
        {
        }
        return out;
    }

    int ScannedFilesOps::count(DatabaseManager &db)
    {
        return countAll(db);
    }

    int ScannedFilesOps::countAll(DatabaseManager &db)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            stmt << std::string(SQL::kCountScannedFiles), Keywords::now;
            RecordSet rs(stmt);
            if (rs.moveFirst())
            {
                return rs[0].convert<int>();
            }
        }
        catch (...)
        {
            // Return 0 on error
        }
        return 0;
    }

    int ScannedFilesOps::countProcessed(DatabaseManager &db)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            stmt << std::string(SQL::kCountProcessedFiles), Keywords::now;
            RecordSet rs(stmt);
            if (rs.moveFirst())
            {
                return rs[0].convert<int>();
            }
        }
        catch (...) {}
        return 0;
    }

    bool ScannedFilesOps::getStatusCounts(DatabaseManager &db, ScanStatusCounts &out)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            // DISC-1: discovery progress is now the flag, not "id below the
            // watermark". The old form counted files that sat below the
            // watermark WITHOUT ever having been compared, so the bar reached
            // 100% while work was still owed.
            stmt << "SELECT COUNT(*), "
                    "COALESCE(SUM(processed=2 OR processed=3),0), "
                    "COALESCE(SUM(processed < 0 AND processed != -2 AND processed != -99),0), "
                    "COALESCE(SUM(processed=2),0), "
                    "COALESCE(SUM(processed=2 AND discovery_done=1),0) "
                    "FROM scanned_files",
                Keywords::into(out.total), Keywords::into(out.processed), Keywords::into(out.errors),
                Keywords::into(out.processed_success), Keywords::into(out.processed_up_to),
                Keywords::now;
            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ScannedFilesOps").error("getStatusCounts failed: %s", std::string(e.what()));
            return false;
        }
    }

    int ScannedFilesOps::countProcessedSuccess(DatabaseManager &db)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            stmt << "SELECT COUNT(*) FROM scanned_files WHERE processed=2", Keywords::now;
            RecordSet rs(stmt);
            if (rs.moveFirst())
            {
                return rs[0].convert<int>();
            }
        }
        catch (...) {}
        return 0;
    }

    int ScannedFilesOps::countDiscovered(DatabaseManager &db)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            stmt << "SELECT COUNT(*) FROM scanned_files WHERE processed = 2 AND discovery_done = 1", Keywords::now;
            RecordSet rs(stmt);
            if (rs.moveFirst())
            {
                return rs[0].convert<int>();
            }
        }
        catch (...) {}
        return 0;
    }

    int ScannedFilesOps::countProcessedAll(DatabaseManager &db)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            stmt << std::string(SQL::kCountProcessedFiles), Keywords::now;
            RecordSet rs(stmt);
            if (rs.moveFirst())
            {
                return rs[0].convert<int>();
            }
        }
        catch (...)
        {
            // Return 0 on error
        }
        return 0;
    }

    // REMOVED: Mode-specific overloads - only EMBEDDING mode supported
    // REMOVED: countErrorAll, countQueuedAll, modeToLinksColumn - mode-specific methods not needed

    bool ScannedFilesOps::markProcessed(DatabaseManager &db, const std::string &file_path, int state)
    {
        // DF-4: a lost state write is not benign — a file left at 1 (picked
        // up) or never reaching 2/-99 is re-queued or re-processed on the next
        // pass — so ride out write-lock contention like the cache writers do.
        const std::string what = "markProcessed(" + file_path + " -> " + std::to_string(state) + ")";
        return DbRetry::withBusyRetry("ScannedFilesOps", what.c_str(), [&]() {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            int flag = state;
            std::string p = file_path;
            stmt << std::string(SQL::kUpdateProcessed), Keywords::use(flag), Keywords::use(p), Keywords::now;

            Poco::Logger::get("ScannedFilesOps").debug("Successfully updated file " + file_path + " to state " + std::to_string(state));
        });
    }

    bool ScannedFilesOps::markIgnored(DatabaseManager &db, const std::string &file_path, const std::string &reason)
    {
        // DF-4: busy-retry, same reasoning as markProcessed (a lost ignore
        // leaves the file at 1 and it is re-processed on the next pass).
        const std::string what = "markIgnored(" + file_path + ")";
        return DbRetry::withBusyRetry("ScannedFilesOps", what.c_str(), [&]() {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            int state = 3; // Ignored
            std::string p = file_path;
            std::string r = reason;
            stmt << "UPDATE scanned_files SET processed=?, ignore_reason=? WHERE file_path=?",
                Keywords::use(state), Keywords::use(r), Keywords::use(p), Keywords::now;
        });
    }

    bool ScannedFilesOps::markProcessedWithEscalation(DatabaseManager &db, const std::string &file_path, int state)
    {
        // DB-7: one UPDATE with the escalation folded into a CASE — the
        // old shape read the FULL 15-column row (getByPath) purely to
        // inspect `processed`, then issued a second statement, doubling
        // the round trips on every processing-failure path. Escalation:
        // an error landing on an existing error becomes state-100
        // (e.g. -1 on -6 -> -101); anything else stores `state` as-is.
        // DF-4: busy-retry around the write (see markProcessed).
        bool found = true;
        const std::string what = "markProcessedWithEscalation(" + file_path + " -> " + std::to_string(state) + ")";
        const bool ok = DbRetry::withBusyRetry("ScannedFilesOps", what.c_str(), [&]() {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();

            int s = state;
            std::string p = file_path;
            Statement stmt(sess);
            stmt << "UPDATE scanned_files SET processed = CASE WHEN processed < 0 AND ? < 0 THEN ? - 100 ELSE ? END "
                    "WHERE file_path = ?",
                Keywords::use(s), Keywords::use(s), Keywords::use(s), Keywords::use(p);
            stmt.execute();

            int changed = 0;
            Statement count(sess);
            count << "SELECT changes()", Keywords::into(changed), Keywords::now;
            found = changed > 0;
        });
        if (ok && !found)
        {
            Poco::Logger::get("ScannedFilesOps").error("File not found in database for escalation: " + file_path);
            return false;
        }
        return ok;
    }

    static std::string joinIds(const std::vector<int> &ids)
    {
        std::string s;
        for (size_t i = 0; i < ids.size(); ++i)
        {
            if (i)
                s += ",";
            s += std::to_string(ids[i]);
        }
        return s;
    }

    bool ScannedFilesOps::setLinks(DatabaseManager &db, const std::string &file_path, const std::vector<int> &link_ids)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            std::string links = joinIds(link_ids);
            std::string p = file_path;
            stmt << std::string(SQL::kUpdateLinks), Keywords::use(links), Keywords::use(p), Keywords::now;
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    // REMOVED: Overloads that infer ServerMode from config - no longer needed

    static std::vector<int> splitIds(const std::string &s)
    {
        std::vector<int> out;
        size_t start = 0;
        while (start < s.size())
        {
            size_t pos = s.find(',', start);
            std::string token = (pos == std::string::npos) ? s.substr(start) : s.substr(start, pos - start);
            if (!token.empty())
            {
                try
                {
                    out.push_back(std::stoi(token));
                }
                catch (...)
                {
                }
            }
            if (pos == std::string::npos)
                break;
            start = pos + 1;
        }
        return out;
    }

    std::vector<int> ScannedFilesOps::getLinks(DatabaseManager &db, const std::string &file_path)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            std::string p = file_path;
            stmt << std::string(SQL::kSelectLinks), Keywords::use(p), Keywords::now;
            RecordSet rs(stmt);
            if (!rs.moveFirst())
                return {};
            std::string links = rs[0].convert<std::string>();
            return splitIds(links);
        }
        catch (...)
        {
            return {};
        }
    }

    std::vector<ScannedFileRow> ScannedFilesOps::listUnprocessed(DatabaseManager &db, int limit)
    {
        std::vector<ScannedFileRow> out;
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            stmt << std::string(SQL::kListUnprocessed);
            if (limit > 0)
            {
                stmt << " LIMIT " << limit;
            }
            stmt.execute();
            RecordSet rs(stmt);
            bool more = rs.moveFirst();
            while (more)
            {
                ScannedFileRow r;
                r.id = rs[0].convert<int>();
                r.file_path = rs[1].convert<std::string>();
                r.relative_path = rs[2].convert<std::string>();
                r.share_name = rs[3].convert<std::string>();
                r.file_name = rs[4].convert<std::string>();
                r.file_metadata = rs[5].convert<std::string>();
                r.processed = rs[6].convert<int>();
                r.links = rs[7].convert<std::string>();
                r.is_network_file = rs[8].convert<int>() != 0;
                r.location_key = rs[9].convert<std::string>();
                r.created_at = rs[10].convert<std::string>();
                if (!rs[11].isEmpty()) r.exif_datetime = rs[11].convert<std::string>();
                if (!rs[12].isEmpty()) r.exif_camera = rs[12].convert<std::string>();
                if (!rs[13].isEmpty()) r.exif_resolution = rs[13].convert<std::string>();
                if (rs.columnCount() > 14 && !rs[14].isEmpty()) r.ignore_reason = rs[14].convert<std::string>();
                out.emplace_back(std::move(r));
                more = rs.moveNext();
            }
        }
        catch (...)
        {
        }
        return out;
    }

    std::vector<ScannedFileRow> ScannedFilesOps::listErroredFiles(DatabaseManager &db)
    {
        std::vector<ScannedFileRow> out;
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            // Select all files where processed < 0
            stmt << "SELECT id, file_path, relative_path, share_name, file_name, file_metadata, processed, links, is_network_file, location_key, created_at, exif_datetime, exif_camera, exif_resolution FROM scanned_files WHERE processed < 0";
            stmt.execute();
            RecordSet rs(stmt);
            bool more = rs.moveFirst();
            while (more)
            {
                ScannedFileRow r;
                r.id = rs[0].convert<int>();
                r.file_path = rs[1].convert<std::string>();
                r.relative_path = rs[2].convert<std::string>();
                r.share_name = rs[3].convert<std::string>();
                r.file_name = rs[4].convert<std::string>();
                r.file_metadata = rs[5].convert<std::string>();
                r.processed = rs[6].convert<int>();
                r.links = rs[7].convert<std::string>();
                r.is_network_file = rs[8].convert<int>() != 0;
                r.location_key = rs[9].convert<std::string>();
                r.created_at = rs[10].convert<std::string>();
                if (!rs[11].isEmpty()) r.exif_datetime = rs[11].convert<std::string>();
                if (!rs[12].isEmpty()) r.exif_camera = rs[12].convert<std::string>();
                if (!rs[13].isEmpty()) r.exif_resolution = rs[13].convert<std::string>();
                if (rs.columnCount() > 14 && !rs[14].isEmpty()) r.ignore_reason = rs[14].convert<std::string>();
                out.emplace_back(std::move(r));
                more = rs.moveNext();
            }
        }
        catch (...)
        {
        }
        return out;
    }

    std::vector<ScannedFileRow> ScannedFilesOps::listIgnoredFiles(DatabaseManager &db)
    {
        std::vector<ScannedFileRow> out;
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            // Select all files where processed = 3
            stmt << "SELECT id, file_path, relative_path, share_name, file_name, file_metadata, processed, links, is_network_file, location_key, created_at, exif_datetime, exif_camera, exif_resolution, ignore_reason FROM scanned_files WHERE processed = 3";
            stmt.execute();
            RecordSet rs(stmt);
            bool more = rs.moveFirst();
            while (more)
            {
                ScannedFileRow r;
                r.id = rs[0].convert<int>();
                r.file_path = rs[1].convert<std::string>();
                r.relative_path = rs[2].convert<std::string>();
                r.share_name = rs[3].convert<std::string>();
                r.file_name = rs[4].convert<std::string>();
                r.file_metadata = rs[5].convert<std::string>();
                r.processed = rs[6].convert<int>();
                r.links = rs[7].convert<std::string>();
                r.is_network_file = rs[8].convert<int>() != 0;
                r.location_key = rs[9].convert<std::string>();
                r.created_at = rs[10].convert<std::string>();
                if (!rs[11].isEmpty()) r.exif_datetime = rs[11].convert<std::string>();
                if (!rs[12].isEmpty()) r.exif_camera = rs[12].convert<std::string>();
                if (!rs[13].isEmpty()) r.exif_resolution = rs[13].convert<std::string>();
                if (rs.columnCount() > 14 && !rs[14].isEmpty()) r.ignore_reason = rs[14].convert<std::string>();
                out.emplace_back(std::move(r));
                more = rs.moveNext();
            }
        }
        catch (...)
        {
        }
        return out;
    }

    bool ScannedFilesOps::updateMetadata(DatabaseManager &db, const std::string &file_path, const std::string &file_metadata)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            std::string p = file_path;
            std::string m = file_metadata;
            stmt << std::string(SQL::kUpdateMetadata), Keywords::use(m), Keywords::use(p), Keywords::now;
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool ScannedFilesOps::updateExifMetadata(DatabaseManager &db, const std::string &file_path,
                                 const ExifInfo &exif)
    {
        // Losing this to write-lock contention silently drops the file's EXIF
        // date, which the day view groups by — the row is never revisited.
        // EXIF-1: Updated to handle comprehensive EXIF fields
        return DbRetry::withBusyRetry("ScannedFilesOps", "updateExifMetadata", [&]() {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);

            // Poco Keywords::use requires non-const lvalues — copy all fields.
            // EXIF-5: absent values become NULL, not 0 — a zero ISO/f-number/
            // exposure is physically impossible and a (0,0) GPS point is a
            // real ocean fix, so zero defaults would corrupt query filters.
            std::string p = file_path;
            std::string dt = exif.datetime;
            std::string cam = exif.camera;
            std::string exif_res = std::to_string(exif.width) + "x" + std::to_string(exif.height);

            auto nullableText = [](const std::string &v) {
                Poco::Nullable<std::string> out;
                if (!v.empty())
                    out = v;
                return out;
            };
            auto nullablePositive = [](double v) {
                Poco::Nullable<double> out;
                if (v > 0.0)
                    out = v;
                return out;
            };

            Poco::Nullable<std::string> make = nullableText(exif.make);
            Poco::Nullable<std::string> model = nullableText(exif.model);
            Poco::Nullable<std::string> lens_model = nullableText(exif.lens_model);
            Poco::Nullable<int> iso;
            if (exif.iso > 0)
                iso = static_cast<int>(exif.iso);
            Poco::Nullable<double> f_number = nullablePositive(exif.f_number);
            Poco::Nullable<double> shutter_apex = nullablePositive(exif.shutter_speed_apex);
            Poco::Nullable<std::string> shutter_text = nullableText(exif.shutter_speed_text);
            Poco::Nullable<double> exposure_time = nullablePositive(exif.exposure_time);
            // 0 EV is a real (and the most common) exposure compensation —
            // it only counts as absent alongside an absent exposure time.
            Poco::Nullable<double> exposure_comp;
            if (exif.exposure_comp != 0.0f || !exposure_time.isNull())
                exposure_comp = static_cast<double>(exif.exposure_comp);
            Poco::Nullable<std::string> metering_mode = nullableText(exif.metering_mode);
            Poco::Nullable<std::string> flash_fired = nullableText(exif.flash_fired);
            Poco::Nullable<std::string> flash_mode = nullableText(exif.flash_mode);
            Poco::Nullable<double> focal_length = nullablePositive(exif.focal_length);
            Poco::Nullable<double> focal_length_35mm = nullablePositive(exif.focal_length_35mm);
            Poco::Nullable<double> focus_distance = nullablePositive(exif.focus_distance);
            Poco::Nullable<std::string> focus_mode = nullableText(exif.focus_mode);
            Poco::Nullable<std::string> white_balance = nullableText(exif.white_balance);
            Poco::Nullable<std::string> color_space = nullableText(exif.color_space);
            Poco::Nullable<std::string> scene_mode = nullableText(exif.scene_mode);
            Poco::Nullable<double> brightness;
            if (exif.brightness != 0.0f)
                brightness = static_cast<double>(exif.brightness);
            Poco::Nullable<std::string> contrast = nullableText(exif.contrast);
            Poco::Nullable<std::string> saturation = nullableText(exif.saturation);
            Poco::Nullable<std::string> sharpness = nullableText(exif.sharpness);
            Poco::Nullable<double> gps_lat, gps_lon, gps_alt;
            if (!std::isnan(exif.gps_lat))
                gps_lat = static_cast<double>(exif.gps_lat);
            if (!std::isnan(exif.gps_lon))
                gps_lon = static_cast<double>(exif.gps_lon);
            if (!std::isnan(exif.gps_alt))
                gps_alt = static_cast<double>(exif.gps_alt);
            int orientation = static_cast<int>(exif.orientation);

            stmt << std::string(SQL::kUpdateExifMetadata),
                Keywords::use(dt),
                Keywords::use(cam),
                Keywords::use(exif_res),
                Keywords::use(make),
                Keywords::use(model),
                Keywords::use(lens_model),
                Keywords::use(iso),
                Keywords::use(f_number),
                Keywords::use(shutter_apex),
                Keywords::use(shutter_text),
                Keywords::use(exposure_time),
                Keywords::use(exposure_comp),
                Keywords::use(metering_mode),
                Keywords::use(flash_fired),
                Keywords::use(flash_mode),
                Keywords::use(focal_length),
                Keywords::use(focal_length_35mm),
                Keywords::use(focus_distance),
                Keywords::use(focus_mode),
                Keywords::use(white_balance),
                Keywords::use(color_space),
                Keywords::use(scene_mode),
                Keywords::use(brightness),
                Keywords::use(contrast),
                Keywords::use(saturation),
                Keywords::use(sharpness),
                Keywords::use(gps_lat),
                Keywords::use(gps_lon),
                Keywords::use(gps_alt),
                Keywords::use(orientation),
                Keywords::use(p), Keywords::now;
        });
    }

    int ScannedFilesOps::clearProcessingFlags(DatabaseManager &db)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();

            // First, count how many files are currently in processing state
            Statement count_stmt(sess);
            int count = 0;
            count_stmt << std::string(SQL::kCountProcessingFiles), Keywords::into(count), Keywords::now;

            if (count == 0)
            {
                Poco::Logger::get("ScannedFilesOps").information("No files in processing state to clear");
                return 0;
            }

            // Now clear the processing flags
            Statement clear_stmt(sess);
            clear_stmt << std::string(SQL::kClearProcessingFlags), Keywords::now;

            Poco::Logger::get("ScannedFilesOps").information("Cleared processing flags for " + std::to_string(count) + " files");

            return count;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ScannedFilesOps").error("Failed to clear processing flags: " + std::string(e.what()));
            return -1; // Return -1 to indicate error
        }
    }

    bool ScannedFilesOps::fileExists(DatabaseManager &db, const std::string &file_path)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();

            int exists = 0;
            std::string file_path_copy = file_path;

            Statement stmt(sess);
            stmt << std::string(SQL::kFileExists),
                Keywords::use(file_path_copy),
                Keywords::into(exists),
                Keywords::now;

            return exists == 1;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ScannedFilesOps").error("Failed to check file existence for " + file_path + ": " + std::string(e.what()));
            return false;
        }
    }

    int ScannedFilesOps::resetAllErrors(DatabaseManager &db)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();

            // First count the error files
            int count = countError(db);
            if (count == 0)
            {
                Poco::Logger::get("ScannedFilesOps").information("No error files to reset");
                return 0;
            }

            // Now reset the errors
            Statement stmt(sess);
            stmt << std::string(SQL::kResetAllErrors), Keywords::now;

            Poco::Logger::get("ScannedFilesOps").information("Reset " + std::to_string(count) + " error files to unprocessed");
            return count;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ScannedFilesOps").error("Failed to reset all errors: " + std::string(e.what()));
            return -1;
        }
    }

    // DB-5: getRegisteredLocationKeys and executeFilteredCount are deleted.
    // Both were dead (zero callers); executeFilteredCount string-concatenated
    // unescaped user-supplied location keys into SQL — an injection surface
    // that also produced wrong counts for any path containing an apostrophe.

    std::string ScannedFilesOps::getLocationKey(DatabaseManager &db, const std::string &file_path)
    {
        auto file_record = getByPath(db, file_path);
        if (file_record.has_value())
        {
            return file_record->location_key;
        }
        return "";
    }

    int ScannedFilesOps::countError(DatabaseManager &db)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            stmt << std::string(SQL::kCountErrorFiles), Keywords::now;
            RecordSet rs(stmt);
            if (rs.moveFirst())
            {
                return rs[0].convert<int>();
            }
        }
        catch (...) {}
        return 0;
    }

    int ScannedFilesOps::countQueued(DatabaseManager &db)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            stmt << std::string(SQL::kCountQueuedFiles), Keywords::now;
            RecordSet rs(stmt);
            if (rs.moveFirst())
            {
                return rs[0].convert<int>();
            }
        }
        catch (...) {}
        return 0;
    }

    int ScannedFilesOps::countErrorAll(DatabaseManager &db)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            int count = 0;
            stmt << std::string(SQL::kCountErrorFiles), Keywords::into(count), Keywords::now;
            return count;
        }
        catch (...)
        {
            return 0;
        }
    }

    int ScannedFilesOps::countQueuedAll(DatabaseManager &db)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            int count = 0;
            stmt << std::string(SQL::kCountQueuedFiles), Keywords::into(count), Keywords::now;
            return count;
        }
        catch (...)
        {
            return 0;
        }
    }

    int ScannedFilesOps::countFilesByLocationKey(DatabaseManager &db, const std::string &location_key)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            std::string key = location_key;
            stmt << std::string(SQL::kCountScannedFilesByLocationKey), Keywords::use(key), Keywords::now;
            RecordSet rs(stmt);
            if (rs.moveFirst())
            {
                return rs[0].convert<int>();
            }
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ScannedFilesOps").error("Error in countFilesByLocationKey: %s", std::string(e.what()));
        }
        catch (...)
        {
            Poco::Logger::get("ScannedFilesOps").error("Unknown error in countFilesByLocationKey");
        }
        return 0;
    }

    std::vector<ScannedFileRow> ScannedFilesOps::getRandomFilesByLocationKey(DatabaseManager &db, const std::string &location_key, int limit)
    {
        std::vector<ScannedFileRow> out;
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            std::string key = location_key;
            int lim = limit;
            stmt << std::string(SQL::kSelectRandomScannedFilesByLocationKey), Keywords::use(key), Keywords::use(lim), Keywords::now;
            RecordSet rs(stmt);
            bool more = rs.moveFirst();
            while (more)
            {
                ScannedFileRow r;
                r.id = rs[0].convert<int>();
                r.file_path = rs[1].convert<std::string>();
                r.relative_path = rs[2].convert<std::string>();
                r.share_name = rs[3].convert<std::string>();
                r.file_name = rs[4].convert<std::string>();
                r.file_metadata = rs[5].convert<std::string>();
                r.processed = rs[6].convert<int>();
                r.links = rs[7].convert<std::string>();
                r.is_network_file = rs[8].convert<int>() != 0;
                r.location_key = rs[9].convert<std::string>();
                r.created_at = rs[10].convert<std::string>();
                if (!rs[11].isEmpty()) r.exif_datetime = rs[11].convert<std::string>();
                if (!rs[12].isEmpty()) r.exif_camera = rs[12].convert<std::string>();
                if (!rs[13].isEmpty()) r.exif_resolution = rs[13].convert<std::string>();
                if (rs.columnCount() > 14 && !rs[14].isEmpty()) r.ignore_reason = rs[14].convert<std::string>();
                out.emplace_back(std::move(r));
                more = rs.moveNext();
            }
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ScannedFilesOps").error("Error in getRandomFilesByLocationKey: %s", std::string(e.what()));
        }
        catch (...)
        {
            Poco::Logger::get("ScannedFilesOps").error("Unknown error in getRandomFilesByLocationKey");
        }
        return out;
    }

    std::vector<ScannedFileRow> ScannedFilesOps::getFilesByLocationKeyBatch(DatabaseManager &db, const std::string &location_key, int limit, int last_id)
    {
        std::vector<ScannedFileRow> out;
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            std::string key = location_key;
            int lim = limit;
            int lid = last_id;
            stmt << std::string(SQL::kSelectScannedFilesByLocationKeyAndIdWithLimit), Keywords::use(key), Keywords::use(lid), Keywords::use(lim), Keywords::now;
            RecordSet rs(stmt);
            bool more = rs.moveFirst();
            while (more)
            {
                ScannedFileRow r;
                r.id = rs[0].convert<int>();
                r.file_path = rs[1].convert<std::string>();
                r.relative_path = rs[2].convert<std::string>();
                r.share_name = rs[3].convert<std::string>();
                r.file_name = rs[4].convert<std::string>();
                r.file_metadata = rs[5].convert<std::string>();
                r.processed = rs[6].convert<int>();
                r.links = rs[7].convert<std::string>();
                r.is_network_file = rs[8].convert<int>() != 0;
                r.location_key = rs[9].convert<std::string>();
                r.created_at = rs[10].convert<std::string>();
                if (!rs[11].isEmpty()) r.exif_datetime = rs[11].convert<std::string>();
                if (!rs[12].isEmpty()) r.exif_camera = rs[12].convert<std::string>();
                if (!rs[13].isEmpty()) r.exif_resolution = rs[13].convert<std::string>();
                if (rs.columnCount() > 14 && !rs[14].isEmpty()) r.ignore_reason = rs[14].convert<std::string>();
                out.emplace_back(std::move(r));
                more = rs.moveNext();
            }
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ScannedFilesOps").error("Error in getFilesByLocationKeyBatch: %s", std::string(e.what()));
        }
        catch (...)
        {
            Poco::Logger::get("ScannedFilesOps").error("Unknown error in getFilesByLocationKeyBatch");
        }
        return out;
    }

    std::vector<ScannedFileRow> ScannedFilesOps::getFilesByLocationKey(DatabaseManager &db, const std::string &location_key)
    {
        Poco::Logger::get("ScannedFilesOps").information("getFilesByLocationKey: Starting query for location_key=%s", location_key);
        std::vector<ScannedFileRow> out;
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            std::string key = location_key;
            stmt << std::string(SQL::kSelectScannedFilesByLocationKey), Keywords::use(key), Keywords::now;
            RecordSet rs(stmt);
            bool more = rs.moveFirst();
            int count = 0;
            while (more)
            {
                ScannedFileRow r;
                r.id = rs[0].convert<int>();
                r.file_path = rs[1].convert<std::string>();
                r.relative_path = rs[2].convert<std::string>();
                r.share_name = rs[3].convert<std::string>();
                r.file_name = rs[4].convert<std::string>();
                r.file_metadata = rs[5].convert<std::string>();
                r.processed = rs[6].convert<int>();
                r.links = rs[7].convert<std::string>();
                r.is_network_file = rs[8].convert<int>() != 0;
                r.location_key = rs[9].convert<std::string>();
                r.created_at = rs[10].convert<std::string>();
                if (!rs[11].isEmpty()) r.exif_datetime = rs[11].convert<std::string>();
                if (!rs[12].isEmpty()) r.exif_camera = rs[12].convert<std::string>();
                if (!rs[13].isEmpty()) r.exif_resolution = rs[13].convert<std::string>();
                if (rs.columnCount() > 14 && !rs[14].isEmpty()) r.ignore_reason = rs[14].convert<std::string>();
                out.emplace_back(std::move(r));
                count++;
                if (count % 10000 == 0)
                {
                    Poco::Logger::get("ScannedFilesOps").information("getFilesByLocationKey: Loaded %d files so far", count);
                }
                more = rs.moveNext();
            }
            Poco::Logger::get("ScannedFilesOps").information("getFilesByLocationKey: Completed, loaded %d files", count);
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ScannedFilesOps").error("Error in getFilesByLocationKey: " + std::string(e.what()));
        }
        catch (...)
        {
            Poco::Logger::get("ScannedFilesOps").error("Unknown error in getFilesByLocationKey");
        }
        return out;
    }

    bool ScannedFilesOps::updateFilePath(DatabaseManager &db,
                                         int file_id,
                                         const std::string &new_path,
                                         const std::string &new_relative_path,
                                         const std::string &new_location_key)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            return updateFilePath(lease.get(), file_id, new_path, new_relative_path, new_location_key);
        }
        catch (...)
        {
            return false;
        }
    }

    bool ScannedFilesOps::updateFilePath(Session &sess,
                                         int file_id,
                                         const std::string &new_path,
                                         const std::string &new_relative_path,
                                         const std::string &new_location_key)
    {
        try
        {
            Statement stmt(sess);
            std::string new_p = new_path;
            std::string new_rel = new_relative_path;
            std::string new_key = new_location_key;
            int id = file_id;
            stmt << std::string(SQL::kUpdateScannedFilePath),
                Keywords::use(new_p),
                Keywords::use(new_rel),
                Keywords::use(new_key),
                Keywords::use(id),
                Keywords::now;
            int rows_affected = stmt.execute();
            if (rows_affected == 0)
            {
                Poco::Logger::get("ScannedFilesOps").warning("No rows updated for file id: %d (file may not exist in database)", file_id);
                return false;
            }
            return true;
        }
        catch (const std::exception &e)
        {
            std::string error_msg = e.what();
            if (error_msg.find("UNIQUE") != std::string::npos ||
                error_msg.find("constraint") != std::string::npos ||
                error_msg.find("Constraint violation") != std::string::npos)
            {
                Poco::Logger::get("ScannedFilesOps").warning("Cannot update file path (id=%d) to %s: path already exists (UNIQUE constraint). ", file_id, new_path);
            }
            else
            {
                Poco::Logger::get("ScannedFilesOps").error("Error updating file path in scanned_files: " + error_msg);
            }
            return false;
        }
    }

    int ScannedFilesOps::updateFilePathInImageArtifacts(DatabaseManager &db,
                                                        const std::string &old_path,
                                                        const std::string &new_path)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            return updateFilePathInImageArtifacts(lease.get(), old_path, new_path);
        }
        catch (...)
        {
            return 0;
        }
    }

    int ScannedFilesOps::updateFilePathInImageArtifacts(Session &sess,
                                                        const std::string &old_path,
                                                        const std::string &new_path)
    {
        try
        {
            Statement stmt(sess);
            std::string new_p = new_path;
            std::string old_p = old_path;
            stmt << std::string(SQL::kUpdateImageArtifactsFilePath),
                Keywords::use(new_p),
                Keywords::use(old_p);
            return stmt.execute();
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ScannedFilesOps").error("Error updating file path in image_artifacts: " + std::string(e.what()));
            return 0;
        }
    }

    int ScannedFilesOps::updateFilePathInProcessingErrors(DatabaseManager &db,
                                                          const std::string &old_path,
                                                          const std::string &new_path)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            return updateFilePathInProcessingErrors(lease.get(), old_path, new_path);
        }
        catch (...)
        {
            return 0;
        }
    }

    int ScannedFilesOps::updateFilePathInProcessingErrors(Session &sess,
                                                          const std::string &old_path,
                                                          const std::string &new_path)
    {
        try
        {
            Statement stmt(sess);
            std::string new_p = new_path;
            std::string old_p = old_path;
            stmt << std::string(SQL::kUpdateProcessingErrorsFilePath),
                Keywords::use(new_p),
                Keywords::use(old_p);
            return stmt.execute();
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ScannedFilesOps").error("Error updating file path in processing_errors: " + std::string(e.what()));
            return 0;
        }
    }

    int ScannedFilesOps::updateFilePathInDuplicateGroups(DatabaseManager &db,
                                                         const std::string &old_path,
                                                         const std::string &new_path)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            return updateFilePathInDuplicateGroups(lease.get(), old_path, new_path);
        }
        catch (...)
        {
            return 0;
        }
    }

    int ScannedFilesOps::updateFilePathInDuplicateGroups(Session &sess,
                                                         const std::string &old_path,
                                                         const std::string &new_path)
    {
        try
        {
            Statement stmt(sess);
            std::string new_p = new_path;
            std::string old_p = old_path;
            stmt << std::string(SQL::kUpdateDuplicateGroupsFilePath),
                Keywords::use(new_p),
                Keywords::use(old_p);
            return stmt.execute();
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ScannedFilesOps").error("Error updating file path in duplicate_groups: " + std::string(e.what()));
            return 0;
        }
    }

    int ScannedFilesOps::updateFilePathInDuplicateMembers(DatabaseManager &db,
                                                          const std::string &old_path,
                                                          const std::string &new_path)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            return updateFilePathInDuplicateMembers(lease.get(), old_path, new_path);
        }
        catch (...)
        {
            return 0;
        }
    }

    int ScannedFilesOps::updateFilePathInDuplicateMembers(Session &sess,
                                                          const std::string &old_path,
                                                          const std::string &new_path)
    {
        try
        {
            Statement stmt(sess);
            std::string new_p = new_path;
            std::string old_p = old_path;
            stmt << std::string(SQL::kUpdateDuplicateMembersFilePath),
                Keywords::use(new_p),
                Keywords::use(old_p);
            return stmt.execute();
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ScannedFilesOps").error("Error updating file path in duplicate_members: " + std::string(e.what()));
            return 0;
        }
    }

    int ScannedFilesOps::updateFilePathInThumbnailCache(DatabaseManager &db,
                                                        const std::string &old_path,
                                                        const std::string &new_path)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            return updateFilePathInThumbnailCache(lease.get(), old_path, new_path);
        }
        catch (...)
        {
            return 0;
        }
    }

    int ScannedFilesOps::updateFilePathInThumbnailCache(Session &sess,
                                                        const std::string &old_path,
                                                        const std::string &new_path)
    {
        try
        {
            Statement stmt(sess);
            std::string new_p = new_path;
            std::string old_p = old_path;
            stmt << std::string(SQL::kUpdateThumbnailCacheSourcePath),
                Keywords::use(new_p),
                Keywords::use(old_p);
            return stmt.execute();
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ScannedFilesOps").error("Error updating file path in thumbnail_cache: " + std::string(e.what()));
            return 0;
        }
    }

    int ScannedFilesOps::updateFilePathInPreviewCache(DatabaseManager &db,
                                                        const std::string &old_path,
                                                        const std::string &new_path)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            return updateFilePathInPreviewCache(lease.get(), old_path, new_path);
        }
        catch (...)
        {
            return 0;
        }
    }

    int ScannedFilesOps::updateFilePathInPreviewCache(Session &sess,
                                                        const std::string &old_path,
                                                        const std::string &new_path)
    {
        try
        {
            Statement stmt(sess);
            std::string new_p = new_path;
            std::string old_p = old_path;
            stmt << std::string(SQL::kUpdatePreviewCacheSourcePath),
                Keywords::use(new_p),
                Keywords::use(old_p);
            return stmt.execute();
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ScannedFilesOps").error("Error updating file path in preview_cache: " + std::string(e.what()));
            return 0;
        }
    }
}
