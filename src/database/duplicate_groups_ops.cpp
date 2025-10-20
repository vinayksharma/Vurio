#include "database/duplicate_groups_ops.hpp"
#include "database/database_manager.hpp"
#include "database/sql_constants.hpp"
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

            // Create indexes
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();

            sess << std::string(SQL::kCreateDuplicateGroupsIndexMode), now;
            sess << std::string(SQL::kCreateDuplicateGroupsIndexCreatedAt), now;
            sess << std::string(SQL::kCreateDuplicateMembersIndexFile), now;
            sess << std::string(SQL::kCreateDuplicateMembersIndexFilePath), now;

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
            Statement stmt(sess);

            std::string mode_str = mode;
            DuplicateGroupRecord rec;

            stmt << std::string(SQL::kSelectDuplicateGroupsByMode),
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
                use(mode_str),
                now;

            while (!stmt.done())
            {
                if (stmt.execute() > 0)
                {
                    results.push_back(rec);
                }
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
            Statement stmt(sess);

            int fid = file_id;
            std::string mode_str = mode;
            DuplicateGroupRecord rec;

            stmt << std::string(SQL::kSelectDuplicateGroupsForFile),
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
                use(fid),
                use(mode_str),
                now;

            while (!stmt.done())
            {
                if (stmt.execute() > 0)
                {
                    results.push_back(rec);
                }
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
                use(rep_flag),
                now;

            Poco::Logger::get("DuplicateGroupsOps").debug("Added member file_id %d to group %d", file_id, group_id);
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
            Statement stmt(sess);

            int gid = group_id;
            DuplicateMemberRecord rec;
            int rep_flag = 0;

            stmt << std::string(SQL::kSelectDuplicateMembersByGroup),
                into(rec.group_id),
                into(rec.file_id),
                into(rec.file_path),
                into(rec.similarity_score),
                into(rec.file_size),
                into(rec.created_date),
                into(rep_flag),
                into(rec.added_at),
                use(gid),
                now;

            while (!stmt.done())
            {
                if (stmt.execute() > 0)
                {
                    rec.is_representative = (rep_flag != 0);
                    results.push_back(rec);
                }
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

            std::string mode_str = mode;

            // Count total groups
            sess << std::string(SQL::kCountDuplicateGroupsByMode), into(stats.total_groups), use(mode_str), now;

            // Count total duplicate members (excluding representatives)
            std::string count_members_query =
                "SELECT COUNT(*) FROM duplicate_members dm "
                "JOIN duplicate_groups dg ON dm.group_id = dg.id "
                "WHERE dg.mode = ?";
            sess << count_members_query, into(stats.total_duplicates), use(mode_str), now;

            // Count unique files with duplicates
            std::string count_files_query =
                "SELECT COUNT(DISTINCT dm.file_id) FROM duplicate_members dm "
                "JOIN duplicate_groups dg ON dm.group_id = dg.id "
                "WHERE dg.mode = ?";
            sess << count_files_query, into(stats.files_with_duplicates), use(mode_str), now;
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

                // Get members for this group
                Statement members_stmt = (sess << std::string(SQL::kSelectDuplicateMembersByGroupId),
                                          use(group.id));
                members_stmt.execute();

                Poco::Data::RecordSet members_rs(members_stmt);
                for (auto &member_row : members_rs)
                {
                    DuplicateMemberRecord member;
                    member.group_id = group.id;
                    member.file_id = member_row[0].convert<int>();
                    member.file_path = member_row[1].convert<std::string>();
                    member.similarity_score = member_row[2].convert<double>();
                    member.file_size = member_row[3].convert<int64_t>();
                    member.created_date = member_row[4].convert<std::string>();
                    member.is_representative = member_row[5].convert<bool>();
                    member.added_at = member_row[6].convert<std::string>();

                    // Only add non-representative members to the candidates list
                    if (!member.is_representative)
                    {
                        group_with_members.members.push_back(member);
                    }
                }

                page.groups.push_back(group_with_members);
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
}
