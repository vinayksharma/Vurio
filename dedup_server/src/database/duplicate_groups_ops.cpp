#include "database/duplicate_groups_ops.hpp"
#include "database/database_manager.hpp"
#include "database/sql_constants.hpp"
#include "database/thumbnail_cache_ops.hpp"
#include "utils/display_geometry.hpp"
#include <Poco/Data/Session.h>
#include <Poco/Data/Statement.h>
#include <Poco/Logger.h>
#include <string>

namespace MediaDedup
{
    using namespace Poco::Data;
    using Poco::Data::Keywords::into;
    using Poco::Data::Keywords::now;
    using Poco::Data::Keywords::use;

    bool DuplicateGroupsOps::ensureTables(DatabaseManager &db)
    {
        Poco::Logger &logger = Poco::Logger::get("DuplicateGroupsOps");

        try
        {
            // Create duplicate_groups table
            if (!db.ensureTableExists("duplicate_groups", SQL::kCreateDuplicateGroupsTable))
            {
                logger.error("Failed to create duplicate_groups table");
                return false;
            }

            // Create duplicate_members table
            if (!db.ensureTableExists("duplicate_members", SQL::kCreateDuplicateMembersTable))
            {
                logger.error("Failed to create duplicate_members table");
                return false;
            }

            // Create duplicate_processing_checkpoint table
            if (!db.ensureTableExists("duplicate_processing_checkpoint", SQL::kCreateDuplicateProcessingCheckpointTable))
            {
                logger.error("Failed to create duplicate_processing_checkpoint table");
                return false;
            }

            // DF-3: persisted similarity edges (union-find re-cluster source)
            if (!db.ensureTableExists("duplicate_edges", SQL::kCreateDuplicateEdgesTable))
            {
                logger.error("Failed to create duplicate_edges table");
                return false;
            }

            // DISC-1: discovery state lives on scanned_files, so it has to
            // exist. Claimed here rather than left to whichever manager happens
            // to initialize first — ScannedFilesOps::ensureTable opens with this
            // same idempotent call and layers its migrations on top, so creating
            // the bare table early costs nothing.
            if (!db.ensureTableExists("scanned_files", SQL::kCreateScannedFilesTable))
            {
                logger.error("Failed to create scanned_files table");
                return false;
            }

            if (!ensureDiscoveryState(db))
                return false;

            // Ensure thumbnail_cache table exists since we now join with it
            if (!ThumbnailCacheOps::ensureTable(db))
            {
                logger.error("Failed to create thumbnail_cache table");
                return false;
            }

            // Create indexes
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();

            sess << std::string(SQL::kDropLegacyDuplicateGroupsIndexMode), now;
            sess << std::string(SQL::kCreateDuplicateGroupsIndexModeRepDate), now;
            sess << std::string(SQL::kCreateDuplicateGroupsIndexCreatedAt), now;
            sess << std::string(SQL::kCreateDuplicateGroupsIndexRepDate), now;
            sess << std::string(SQL::kCreateDuplicateGroupsIndexRepPath), now;
            sess << std::string(SQL::kCreateDuplicateGroupsIndexRepFileId), now;
            sess << std::string(SQL::kCreateDuplicateMembersIndexFile), now;
            sess << std::string(SQL::kCreateDuplicateMembersIndexFilePath), now;
            sess << std::string(SQL::kCreateDuplicateMembersIndexRepGroupFile), now;
            sess << std::string(SQL::kCreateDuplicateEdgesIndexScore), now;

            logger.debug("Successfully ensured all duplicate detection tables and indexes");
            return true;
        }
        catch (const std::exception &e)
        {
            logger.error("Exception in ensureTables: %s", std::string(e.what()));
            return false;
        }
        catch (...)
        {
            logger.error("Unknown exception in ensureTables");
            return false;
        }
    }

    bool DuplicateGroupsOps::ensureDiscoveryState(DatabaseManager &db)
    {
        Poco::Logger &logger = Poco::Logger::get("DuplicateGroupsOps");

        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();

            // Fresh databases get discovery_done from kCreateScannedFilesTable;
            // pre-existing ones get it here. NOT NULL with a constant default,
            // so the ALTER is a schema-only change (no table rewrite).
            try
            {
                sess << std::string(SQL::kAlterScannedFilesAddDiscoveryDone), now;
                logger.information("Added discovery_done column to scanned_files table.");
            }
            catch (const std::exception &)
            {
                // Column already exists.
            }

            sess << std::string(SQL::kCreateScannedFilesIndexPendingDiscovery), now;

            // DISC-1 migration. The old stopgap's own artifacts are the guard:
            // if either is still here, this database predates per-file state,
            // and dropping them below makes the migration self-clearing. A
            // fresh database has neither, so nothing runs and every file starts
            // at 0 — which is correct, it has nothing discovered yet.
            int legacy_requeue = 0;
            {
                Statement q(sess);
                q << "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' "
                     "AND name = 'duplicate_discovery_requeue'",
                    into(legacy_requeue), now;
            }
            if (legacy_requeue > 0)
            {
                // The requeue table has to still exist for the NOT IN subquery,
                // so migrate before dropping.
                sess << std::string(SQL::kMigrateDiscoveryDoneFromWatermark), now;
                int migrated = 0;
                Statement c(sess);
                c << "SELECT changes()", into(migrated), now;
                logger.information(
                    "DISC-1: migrated %d file(s) to discovery_done=1 from the id watermark; "
                    "requeued ids and everything past the watermark stay pending.", migrated);

                sess << std::string(SQL::kDropRequeueLateProcessedTrigger), now;
                sess << std::string(SQL::kDropDuplicateDiscoveryRequeueTable), now;
                logger.information("DISC-1: dropped the discovery requeue table and late-processed trigger.");
            }
            else
            {
                // Table already gone; the trigger cannot outlive it (its body
                // inserts there), but drop defensively so an interrupted
                // migration cannot leave one behind that fails every write.
                sess << std::string(SQL::kDropRequeueLateProcessedTrigger), now;
            }
            return true;
        }
        catch (const std::exception &e)
        {
            logger.error("Exception in ensureDiscoveryState: %s", std::string(e.what()));
            return false;
        }
    }

    int DuplicateGroupsOps::createGroup(DatabaseManager &db,
                                        const std::string &mode,
                                        int representative_file_id,
                                        const std::string &representative_file_path,
                                        int64_t representative_file_size,
                                        const std::string &representative_created_date,
                                        double similarity_threshold)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);

            std::string mode_str = mode;
            int rep_id = representative_file_id;
            std::string rep_path = representative_file_path;
            int64_t rep_size = representative_file_size;
            std::string rep_date = representative_created_date;
            double threshold = similarity_threshold;
            int member_count = 1; // Initially just the representative

            stmt << std::string(SQL::kInsertDuplicateGroup),
                use(mode_str),
                use(rep_id),
                use(rep_path),
                use(rep_size),
                use(rep_date),
                use(threshold),
                use(member_count),
                now;

            // Get the last inserted row id
            int group_id = 0;
            sess << "SELECT last_insert_rowid()", into(group_id), now;

            Poco::Logger::get("DuplicateGroupsOps").debug("Created duplicate group %d for mode %s", group_id, mode);
            return group_id;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("DuplicateGroupsOps").error("Exception in createGroup: %s", std::string(e.what()));
            return -1;
        }
        catch (...)
        {
            Poco::Logger::get("DuplicateGroupsOps").error("Unknown exception in createGroup");
            return -1;
        }
    }

    bool DuplicateGroupsOps::updateGroupRepresentative(DatabaseManager &db,
                                                       int group_id,
                                                       int new_representative_file_id,
                                                       const std::string &new_representative_file_path,
                                                       int64_t new_representative_file_size,
                                                       const std::string &new_representative_created_date,
                                                       int member_count)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);

            int rep_id = new_representative_file_id;
            std::string rep_path = new_representative_file_path;
            int64_t rep_size = new_representative_file_size;
            std::string rep_date = new_representative_created_date;
            int count = member_count;
            int gid = group_id;

            stmt << std::string(SQL::kUpdateDuplicateGroupRepresentative),
                use(rep_id),
                use(rep_path),
                use(rep_size),
                use(rep_date),
                use(count),
                use(gid),
                now;

            Poco::Logger::get("DuplicateGroupsOps").debug("Updated group %d representative to file_id %d", group_id, new_representative_file_id);
            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("DuplicateGroupsOps").error("Exception in updateGroupRepresentative: %s", std::string(e.what()));
            return false;
        }
        catch (...)
        {
            Poco::Logger::get("DuplicateGroupsOps").error("Unknown exception in updateGroupRepresentative");
            return false;
        }
    }

    bool DuplicateGroupsOps::deleteGroup(DatabaseManager &db, int group_id)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);

            int gid = group_id;
            stmt << std::string(SQL::kDeleteDuplicateGroup), use(gid), now;

            Poco::Logger::get("DuplicateGroupsOps").debug("Deleted duplicate group %d", group_id);
            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("DuplicateGroupsOps").error("Exception in deleteGroup: %s", std::string(e.what()));
            return false;
        }
        catch (...)
        {
            Poco::Logger::get("DuplicateGroupsOps").error("Unknown exception in deleteGroup");
            return false;
        }
    }

    bool DuplicateGroupsOps::deleteGroupsByMode(DatabaseManager &db, const std::string &mode)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();

            // First delete all members for groups in this mode
            Statement delete_members(sess);
            std::string mode_str = mode;
            delete_members << "DELETE FROM duplicate_members WHERE group_id IN "
                              "(SELECT id FROM duplicate_groups WHERE mode = ?)",
                use(mode_str), now;

            // Then delete the groups themselves
            Statement delete_groups(sess);
            delete_groups << "DELETE FROM duplicate_groups WHERE mode = ?",
                use(mode_str), now;

            Poco::Logger::get("DuplicateGroupsOps").information("Deleted all duplicate groups for mode: %s", mode);
            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("DuplicateGroupsOps").error("Exception in deleteGroupsByMode: %s", std::string(e.what()));
            return false;
        }
        catch (...)
        {
            Poco::Logger::get("DuplicateGroupsOps").error("Unknown exception in deleteGroupsByMode");
            return false;
        }
    }

    bool DuplicateGroupsOps::resetCheckpoint(DatabaseManager &db, const std::string &mode)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);

            std::string mode_str = mode;
            int zero = 0;

            stmt << "INSERT INTO duplicate_processing_checkpoint "
                    "(mode, last_processed_id, files_checked, duplicates_found, groups_created, groups_updated, last_run_timestamp) "
                    "VALUES(?, ?, ?, ?, ?, ?, CURRENT_TIMESTAMP) "
                    "ON CONFLICT(mode) DO UPDATE SET "
                    "  last_processed_id = 0, "
                    "  files_checked = 0, "
                    "  duplicates_found = 0, "
                    "  groups_created = 0, "
                    "  groups_updated = 0, "
                    "  last_run_timestamp = CURRENT_TIMESTAMP",
                use(mode_str), use(zero), use(zero), use(zero), use(zero), use(zero), now;

            // DISC-1: this IS the "start discovery over" primitive (the reset
            // API, and the floor-provenance reset in DuplicateFinder). Discovery
            // state now lives per file, so zeroing the checkpoint alone would
            // reset nothing and the next run would find no work.
            sess << std::string(SQL::kClearDiscoveryDoneAll), now;

            Poco::Logger::get("DuplicateGroupsOps").information("Reset checkpoint for mode: %s", mode);
            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("DuplicateGroupsOps").error("Exception in resetCheckpoint: %s", std::string(e.what()));
            return false;
        }
        catch (...)
        {
            Poco::Logger::get("DuplicateGroupsOps").error("Unknown exception in resetCheckpoint");
            return false;
        }
    }

    std::optional<DuplicateGroupRecord> DuplicateGroupsOps::getGroupById(DatabaseManager &db, int group_id)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);

            int gid = group_id;
            DuplicateGroupRecord rec;

            stmt << std::string(SQL::kSelectDuplicateGroupById),
                into(rec.id),
                into(rec.mode),
                into(rec.representative_file_id),
                into(rec.representative_file_path),
                into(rec.representative_file_size),
                into(rec.representative_created_date),
                into(rec.similarity_threshold),
                into(rec.member_count),
                into(rec.created_at),
                into(rec.updated_at),
                use(gid),
                now;

            if (stmt.rowsExtracted() > 0)
            {
                return rec;
            }
            return std::nullopt;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("DuplicateGroupsOps").error("Exception in getGroupById: %s", std::string(e.what()));
            return std::nullopt;
        }
        catch (...)
        {
            Poco::Logger::get("DuplicateGroupsOps").error("Unknown exception in getGroupById");
            return std::nullopt;
        }
    }

    std::vector<DuplicateGroupRecord> DuplicateGroupsOps::getGroupsByMode(DatabaseManager &db, const std::string &mode)
    {
        std::vector<DuplicateGroupRecord> results;
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();

            std::string mode_str = mode;

            // Use RecordSet for more robust extraction
            Statement stmt = (sess << std::string(SQL::kSelectDuplicateGroupsByMode), use(mode_str));
            stmt.execute();

            Poco::Data::RecordSet rs(stmt);
            for (auto &row : rs)
            {
                DuplicateGroupRecord rec;
                rec.id = row[0].convert<int>();
                rec.mode = row[1].convert<std::string>();
                rec.representative_file_id = row[2].convert<int>();
                rec.representative_file_path = row[3].convert<std::string>();
                rec.representative_file_size = row[4].convert<int64_t>();
                rec.representative_created_date = row[5].convert<std::string>();
                rec.similarity_threshold = row[6].convert<double>();
                rec.member_count = row[7].convert<int>();
                rec.created_at = row[8].convert<std::string>();
                rec.updated_at = row[9].convert<std::string>();

                results.push_back(rec);
            }
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("DuplicateGroupsOps").error("Exception in getGroupsByMode: %s", std::string(e.what()));
        }
        catch (...)
        {
            Poco::Logger::get("DuplicateGroupsOps").error("Unknown exception in getGroupsByMode");
        }
        return results;
    }

    std::vector<DuplicateGroupRecord> DuplicateGroupsOps::getGroupsForFile(DatabaseManager &db,
                                                                           int file_id,
                                                                           const std::string &mode)
    {
        std::vector<DuplicateGroupRecord> results;
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();

            int fid = file_id;
            int fid2 = file_id;
            std::string mode_str = mode;

            // Use RecordSet for more robust extraction
            Statement stmt = (sess << std::string(SQL::kSelectDuplicateGroupsForFile), use(fid), use(fid2), use(mode_str));
            stmt.execute();

            Poco::Data::RecordSet rs(stmt);
            for (auto &row : rs)
            {
                DuplicateGroupRecord rec;
                rec.id = row[0].convert<int>();
                rec.mode = row[1].convert<std::string>();
                rec.representative_file_id = row[2].convert<int>();
                rec.representative_file_path = row[3].convert<std::string>();
                rec.representative_file_size = row[4].convert<int64_t>();
                rec.representative_created_date = row[5].convert<std::string>();
                rec.similarity_threshold = row[6].convert<double>();
                rec.member_count = row[7].convert<int>();
                rec.created_at = row[8].convert<std::string>();
                rec.updated_at = row[9].convert<std::string>();

                results.push_back(rec);
            }
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("DuplicateGroupsOps").error("Exception in getGroupsForFile: %s", std::string(e.what()));
        }
        catch (...)
        {
            Poco::Logger::get("DuplicateGroupsOps").error("Unknown exception in getGroupsForFile");
        }
        return results;
    }

    bool DuplicateGroupsOps::addMember(DatabaseManager &db,
                                       int group_id,
                                       int file_id,
                                       const std::string &file_path,
                                       double similarity_score,
                                       int64_t file_size,
                                       const std::string &created_date,
                                       bool is_representative)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);

            int gid = group_id;
            int fid = file_id;
            std::string path = file_path;
            double score = similarity_score;
            int64_t size = file_size;
            std::string date = created_date;
            int rep_flag = is_representative ? 1 : 0;

            stmt << std::string(SQL::kInsertDuplicateMember),
                use(gid),
                use(fid),
                use(path),
                use(score),
                use(size),
                use(date),
                use(rep_flag);

            int rows_affected = stmt.execute();
            if (rows_affected > 0)
            {
                Poco::Logger::get("DuplicateGroupsOps").debug("Added member file_id %d to group %d", file_id, group_id);
            }
            else
            {
                Poco::Logger::get("DuplicateGroupsOps").debug("Member file_id %d already exists in group %d, skipping insert", file_id, group_id);
            }
            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("DuplicateGroupsOps").error("Exception in addMember: %s", std::string(e.what()));
            return false;
        }
        catch (...)
        {
            Poco::Logger::get("DuplicateGroupsOps").error("Unknown exception in addMember");
            return false;
        }
    }

    bool DuplicateGroupsOps::updateMemberRepresentativeFlag(DatabaseManager &db,
                                                            int group_id,
                                                            int file_id,
                                                            bool is_representative)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);

            int rep_flag = is_representative ? 1 : 0;
            int gid = group_id;
            int fid = file_id;

            stmt << std::string(SQL::kUpdateDuplicateMemberRepresentativeFlag),
                use(rep_flag),
                use(gid),
                use(fid),
                now;

            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("DuplicateGroupsOps").error("Exception in updateMemberRepresentativeFlag: %s", std::string(e.what()));
            return false;
        }
        catch (...)
        {
            Poco::Logger::get("DuplicateGroupsOps").error("Unknown exception in updateMemberRepresentativeFlag");
            return false;
        }
    }

    bool DuplicateGroupsOps::deleteMember(DatabaseManager &db, int group_id, int file_id)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);

            int gid = group_id;
            int fid = file_id;

            stmt << std::string(SQL::kDeleteDuplicateMember), use(gid), use(fid), now;

            Poco::Logger::get("DuplicateGroupsOps").debug("Deleted member file_id %d from group %d", file_id, group_id);
            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("DuplicateGroupsOps").error("Exception in deleteMember: %s", std::string(e.what()));
            return false;
        }
        catch (...)
        {
            Poco::Logger::get("DuplicateGroupsOps").error("Unknown exception in deleteMember");
            return false;
        }
    }

    std::vector<DuplicateMemberRecord> DuplicateGroupsOps::getMembersByGroup(DatabaseManager &db, int group_id)
    {
        std::vector<DuplicateMemberRecord> results;
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();

            int gid = group_id;

            // Use RecordSet for more robust extraction
            Statement stmt = (sess << std::string(SQL::kSelectDuplicateMembersByGroup), use(gid));
            stmt.execute();

            Poco::Data::RecordSet rs(stmt);
            for (auto &row : rs)
            {
                DuplicateMemberRecord rec;
                rec.group_id = row[0].convert<int>();
                rec.file_id = row[1].convert<int>();
                rec.file_path = row[2].convert<std::string>();
                rec.similarity_score = row[3].convert<double>();
                rec.file_size = row[4].convert<int64_t>();
                rec.created_date = row[5].convert<std::string>();
                rec.is_representative = row[6].convert<bool>();
                rec.added_at = row[7].convert<std::string>();
                if (!row[8].isEmpty())
                    rec.cached_thumbnail_path = row[8].convert<std::string>();
                if (!row[9].isEmpty())
                {
                    DisplayDims dims = displayDimensionsFromExifResolution(row[9].convert<std::string>());
                    rec.display_width = dims.width;
                    rec.display_height = dims.height;
                }

                results.push_back(rec);
            }
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("DuplicateGroupsOps").error("Exception in getMembersByGroup: %s", std::string(e.what()));
        }
        catch (...)
        {
            Poco::Logger::get("DuplicateGroupsOps").error("Unknown exception in getMembersByGroup");
        }
        return results;
    }

    bool DuplicateGroupsOps::removeMember(DatabaseManager &db, int group_id, int file_id)
    {
        // Alias for deleteMember for semantic clarity
        return deleteMember(db, group_id, file_id);
    }

    bool DuplicateGroupsOps::updateGroupMemberCount(DatabaseManager &db, int group_id, int member_count)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);

            int gid = group_id;
            int count = member_count;

            stmt << "UPDATE duplicate_groups SET member_count = ?, updated_at = CURRENT_TIMESTAMP WHERE id = ?",
                use(count), use(gid), now;

            int rows_affected = stmt.execute();
            if (rows_affected > 0)
            {
                Poco::Logger::get("DuplicateGroupsOps").debug("Updated group %d member count to %d", group_id, member_count);
                return true;
            }
            else
            {
                Poco::Logger::get("DuplicateGroupsOps").warning("No group found with id %d for member count update", group_id);
                return false;
            }
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("DuplicateGroupsOps").error("Exception in updateGroupMemberCount: %s", std::string(e.what()));
            return false;
        }
        catch (...)
        {
            Poco::Logger::get("DuplicateGroupsOps").error("Unknown exception in updateGroupMemberCount");
            return false;
        }
    }

    int DuplicateGroupsOps::syncGroupMemberCount(DatabaseManager &db, int group_id)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            int gid = group_id;

            // Calculate true distinct count using UNION of members and representative
            int actual_count = 0;
            Statement count_stmt(sess);
            count_stmt << "SELECT COUNT(DISTINCT file_id) FROM ("
                          "  SELECT file_id FROM duplicate_members dm WHERE dm.group_id = ? "
                          "  UNION "
                          "  SELECT representative_file_id FROM duplicate_groups dg WHERE dg.id = ?"
                          ")",
                into(actual_count), use(gid), use(gid);
            count_stmt.execute();

            if (actual_count <= 1)
            {
                Poco::Logger::get("DuplicateGroupsOps").information("Group %d has true member count %d. Purging group.", group_id, actual_count);
                deleteGroup(db, group_id);
                return actual_count; // The group is gone
            }
            else
            {
                // Synchronize the DB field
                Statement update_stmt(sess);
                update_stmt << "UPDATE duplicate_groups SET member_count = ?, updated_at = CURRENT_TIMESTAMP WHERE id = ?",
                    use(actual_count), use(gid), now;
                update_stmt.execute();
                
                Poco::Logger::get("DuplicateGroupsOps").debug("Synchronized group %d member count to %d", group_id, actual_count);
                return actual_count;
            }
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("DuplicateGroupsOps").error("Exception in syncGroupMemberCount: %s", std::string(e.what()));
            return 0;
        }
        catch (...)
        {
            Poco::Logger::get("DuplicateGroupsOps").error("Unknown exception in syncGroupMemberCount");
            return 0;
        }
    }

    int DuplicateGroupsOps::countGroupsByMode(DatabaseManager &db, const std::string &mode)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);

            std::string mode_str = mode;
            int count = 0;

            stmt << std::string(SQL::kCountDuplicateGroupsByMode), into(count), use(mode_str), now;

            return count;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("DuplicateGroupsOps").error("Exception in countGroupsByMode: %s", std::string(e.what()));
            return 0;
        }
        catch (...)
        {
            Poco::Logger::get("DuplicateGroupsOps").error("Unknown exception in countGroupsByMode");
            return 0;
        }
    }

    int DuplicateGroupsOps::getDuplicateGroupsCountOlderThan(DatabaseManager &db, const std::string &mode, const std::string &date)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);

            std::string mode_str = mode;
            std::string date_str = date;
            int count = 0;

            stmt << std::string(SQL::kSelectDuplicateGroupsCountOlderThan), into(count), use(mode_str), use(date_str), now;

            return count;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("DuplicateGroupsOps").error("Exception in getDuplicateGroupsCountOlderThan: %s", std::string(e.what()));
            return 0;
        }
        catch (...)
        {
            Poco::Logger::get("DuplicateGroupsOps").error("Unknown exception in getDuplicateGroupsCountOlderThan");
            return 0;
        }
    }

    int DuplicateGroupsOps::getGroupRank(DatabaseManager &db, int group_id)
    {
        try
        {
            auto group = getGroupById(db, group_id);
            if (!group.has_value())
                return -1;

            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);

            std::string date_str = group->representative_created_date;
            int gid = group_id;
            int rank = 0;

            stmt << std::string(SQL::kSelectDuplicateGroupRank),
                into(rank), use(date_str), use(date_str), use(gid), now;

            return rank;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("DuplicateGroupsOps").error("Exception in getGroupRank: %s", std::string(e.what()));
            return -1;
        }
        catch (...)
        {
            Poco::Logger::get("DuplicateGroupsOps").error("Unknown exception in getGroupRank");
            return -1;
        }
    }

    int DuplicateGroupsOps::countMembersByGroup(DatabaseManager &db, int group_id)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);

            int gid = group_id;
            int count = 0;

            stmt << std::string(SQL::kCountDuplicateMembersByGroup), into(count), use(gid), now;

            return count;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("DuplicateGroupsOps").error("Exception in countMembersByGroup: %s", std::string(e.what()));
            return 0;
        }
        catch (...)
        {
            Poco::Logger::get("DuplicateGroupsOps").error("Unknown exception in countMembersByGroup");
            return 0;
        }
    }

    DuplicateStats DuplicateGroupsOps::getStats(DatabaseManager &db, const std::string &mode)
    {
        DuplicateStats stats;
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);

            std::string mode_str = mode;

            // Count total groups
            {
                Statement stmt1(sess);
                stmt1 << std::string(SQL::kCountDuplicateGroupsByMode), into(stats.total_groups), use(mode_str), now;
            }

            // Count total duplicate members (excluding representatives)
            {
                std::string count_members_query =
                    "SELECT COUNT(*) FROM duplicate_members dm "
                    "JOIN duplicate_groups dg ON dm.group_id = dg.id "
                    "WHERE dg.mode = ?";
                Statement stmt2(sess);
                stmt2 << count_members_query, into(stats.total_duplicates), use(mode_str), now;
            }

            // Count unique files with duplicates
            {
                std::string count_files_query =
                    "SELECT COUNT(DISTINCT dm.file_id) FROM duplicate_members dm "
                    "JOIN duplicate_groups dg ON dm.group_id = dg.id "
                    "WHERE dg.mode = ?";
                Statement stmt3(sess);
                stmt3 << count_files_query, into(stats.files_with_duplicates), use(mode_str), now;
            }
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("DuplicateGroupsOps").error("Exception in getStats: %s", std::string(e.what()));
        }
        catch (...)
        {
            Poco::Logger::get("DuplicateGroupsOps").error("Unknown exception in getStats");
        }
        return stats;
    }

    bool DuplicateGroupsOps::upsertCheckpoint(DatabaseManager &db,
                                              const std::string &mode,
                                              int last_processed_id,
                                              int files_checked,
                                              int duplicates_found,
                                              int groups_created,
                                              int groups_updated)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);

            std::string mode_str = mode;
            int last_id = last_processed_id;
            int checked = files_checked;
            int found = duplicates_found;
            int created = groups_created;
            int updated = groups_updated;

            stmt << std::string(SQL::kUpsertDuplicateCheckpoint),
                use(mode_str),
                use(last_id),
                use(checked),
                use(found),
                use(created),
                use(updated),
                now;

            Poco::Logger::get("DuplicateGroupsOps").debug("Updated checkpoint for mode %s: last_id=%d", mode, last_processed_id);
            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("DuplicateGroupsOps").error("Exception in upsertCheckpoint: %s", std::string(e.what()));
            return false;
        }
        catch (...)
        {
            Poco::Logger::get("DuplicateGroupsOps").error("Unknown exception in upsertCheckpoint");
            return false;
        }
    }

    std::optional<DuplicateCheckpointRecord> DuplicateGroupsOps::getCheckpoint(DatabaseManager &db,
                                                                               const std::string &mode)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);

            std::string mode_str = mode;
            DuplicateCheckpointRecord rec;

            stmt << std::string(SQL::kSelectDuplicateCheckpoint),
                into(rec.mode),
                into(rec.last_processed_id),
                into(rec.last_run_timestamp),
                into(rec.files_checked),
                into(rec.duplicates_found),
                into(rec.groups_created),
                into(rec.groups_updated),
                use(mode_str),
                now;

            if (stmt.rowsExtracted() > 0)
            {
                return rec;
            }
            return std::nullopt;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("DuplicateGroupsOps").error("Exception in getCheckpoint: %s", std::string(e.what()));
            return std::nullopt;
        }
        catch (...)
        {
            Poco::Logger::get("DuplicateGroupsOps").error("Unknown exception in getCheckpoint");
            return std::nullopt;
        }
    }

    DuplicateGroupsPage DuplicateGroupsOps::getGroupsWithMembers(DatabaseManager &db, int start, int limit, const std::string &mode)
    {
        Poco::Logger &logger = Poco::Logger::get("DuplicateGroupsOps");
        DuplicateGroupsPage page;
        page.start = start;
        page.end = start + limit;

        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();

            // Create non-const copy for Poco::Data binding (use() requires non-const)
            std::string mode_copy = mode;

            // Get total count (filtered by mode if provided)
            if (mode.empty())
            {
                Statement count_stmt = (sess << std::string(SQL::kSelectDuplicateGroupsCount), into(page.total_count));
                count_stmt.execute();
            }
            else
            {
                Statement count_stmt = (sess << std::string(SQL::kSelectDuplicateGroupsCountByMode), into(page.total_count), use(mode_copy));
                count_stmt.execute();
            }

            logger.debug("Total duplicate groups count (mode=%s): %d", mode.empty() ? "all" : mode.c_str(), page.total_count);

            // Get paginated groups (filtered by mode if provided)
            Statement groups_stmt(sess);
            if (mode.empty())
            {
                groups_stmt = (sess << std::string(SQL::kSelectDuplicateGroupsWithPagination),
                               use(limit),
                               use(start));
            }
            else
            {
                groups_stmt = (sess << std::string(SQL::kSelectDuplicateGroupsWithPaginationByMode),
                               use(mode_copy),
                               use(limit),
                               use(start));
            }
            groups_stmt.execute();

            Poco::Data::RecordSet rs(groups_stmt);

            // DB-7: collect the page's groups first, then fetch ALL their
            // members in ONE set-based query — the old shape issued one
            // members statement per group (N+1: ~100 prepare/execute cycles
            // per page where a single IN(...) query takes ~1 ms).
            std::map<int, std::size_t> group_index; // group id -> page.groups slot
            std::string id_list;
            for (auto &row : rs)
            {
                DuplicateGroupWithMembers group_with_members;
                DuplicateGroupRecord &group = group_with_members.group;

                // Extract group data
                group.id = row[0].convert<int>();
                group.mode = row[1].convert<std::string>();
                group.representative_file_id = row[2].convert<int>();
                group.representative_file_path = row[3].convert<std::string>();
                group.representative_file_size = row[4].convert<int64_t>();
                group.representative_created_date = row[5].convert<std::string>();
                group.similarity_threshold = row[6].convert<double>();
                group.member_count = row[7].convert<int>();
                group.created_at = row[8].convert<std::string>();
                group.updated_at = row[9].convert<std::string>();

                group_index[group.id] = page.groups.size();
                if (!id_list.empty())
                    id_list += ",";
                id_list += std::to_string(group.id);
                page.groups.push_back(std::move(group_with_members));
            }

            if (!page.groups.empty())
            {
                // Integer ids composed directly (no injection surface); the
                // ORDER BY keeps each group's members similarity-sorted, the
                // contract the per-group query provided.
                // THUMB-1: LEFT JOIN scanned_files for exif_resolution (index
                // 9); LEFT so a member whose scanned_files row was deleted is
                // still returned (it renders as a missing/unavailable tile).
                std::string members_query =
                    "SELECT dm.group_id, dm.file_id, dm.file_path, dm.similarity_score, dm.file_size, dm.created_date, "
                    "dm.is_representative, dm.added_at, "
                    VURIO_BEST_THUMB_SUBQUERY("dm.file_path") " AS cached_path, "
                    "sf.exif_resolution AS exif_resolution "
                    "FROM duplicate_members dm "
                    "LEFT JOIN scanned_files sf ON sf.id = dm.file_id "
                    "WHERE dm.group_id IN (" + id_list + ") ORDER BY dm.group_id, dm.similarity_score DESC";

                Statement members_stmt(sess);
                members_stmt << members_query;
                members_stmt.execute();

                Poco::Data::RecordSet members_rs(members_stmt);
                for (auto &member_row : members_rs)
                {
                    DuplicateMemberRecord member;
                    member.group_id = member_row[0].convert<int>();
                    member.file_id = member_row[1].convert<int>();
                    member.file_path = member_row[2].convert<std::string>();
                    member.similarity_score = member_row[3].convert<double>();
                    member.file_size = member_row[4].convert<int64_t>();
                    member.created_date = member_row[5].convert<std::string>();
                    member.is_representative = member_row[6].convert<bool>();
                    member.added_at = member_row[7].convert<std::string>();

                    if (!member_row[8].isEmpty()) {
                        member.cached_thumbnail_path = member_row[8].convert<std::string>();
                    }
                    if (!member_row[9].isEmpty()) {
                        DisplayDims dims = displayDimensionsFromExifResolution(member_row[9].convert<std::string>());
                        member.display_width = dims.width;
                        member.display_height = dims.height;
                    }

                    auto it = group_index.find(member.group_id);
                    if (it != group_index.end())
                    {
                        // Always add the member, including the representative,
                        // so the UI receives full metadata
                        page.groups[it->second].members.push_back(std::move(member));
                    }
                }
            }

            page.returned = static_cast<int>(page.groups.size());
            logger.debug("Retrieved %d groups (start=%d, limit=%d)", page.returned, start, limit);

            return page;
        }
        catch (const std::exception &e)
        {
            logger.error("Exception in getGroupsWithMembers: %s", std::string(e.what()));
            return page; // Return empty page on error
        }
    }

    std::vector<std::string> DuplicateGroupsOps::getEjectCandidatePaths(DatabaseManager &db, double tightness)
    {
        std::vector<std::string> results;
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();

            double t = tightness;
            Statement stmt = (sess << std::string(SQL::kSelectEjectCandidateMemberPaths), use(t));
            stmt.execute();

            Poco::Data::RecordSet rs(stmt);
            for (auto &row : rs)
            {
                results.push_back(row[0].convert<std::string>());
            }
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("DuplicateGroupsOps").error("Exception in getEjectCandidatePaths: %s", std::string(e.what()));
        }
        return results;
    }

    std::vector<std::string> DuplicateGroupsOps::getAllMemberPaths(DatabaseManager &db)
    {
        std::vector<std::string> results;
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();

            Statement stmt = (sess << std::string(SQL::kSelectAllDuplicateMemberPaths));
            stmt.execute();

            Poco::Data::RecordSet rs(stmt);
            for (auto &row : rs)
            {
                results.push_back(row[0].convert<std::string>());
            }
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("DuplicateGroupsOps").error("Exception in getAllMemberPaths: %s", std::string(e.what()));
        }
        return results;
    }
}
