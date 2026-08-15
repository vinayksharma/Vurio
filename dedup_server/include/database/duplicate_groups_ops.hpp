#pragma once

#include <string>
#include <vector>
#include <optional>
#include "config/config_enums.hpp"

namespace MediaDedup
{
    class DatabaseManager;

    struct DuplicateGroupRecord
    {
        int id = 0;
        std::string mode;
        int representative_file_id = 0;
        std::string representative_file_path;
        int64_t representative_file_size = 0;
        std::string representative_created_date;
        double similarity_threshold = 0.0;
        int member_count = 0;
        std::string created_at;
        std::string updated_at;
    };

    struct DuplicateMemberRecord
    {
        int group_id = 0;
        int file_id = 0;
        std::string file_path;
        double similarity_score = 0.0;
        int64_t file_size = 0;
        std::string created_date;
        bool is_representative = false;
        std::string added_at;
        std::string cached_thumbnail_path;
    };

    struct DuplicateCheckpointRecord
    {
        std::string mode;
        int last_processed_id = 0;
        std::string last_run_timestamp;
        int files_checked = 0;
        int duplicates_found = 0;
        int groups_created = 0;
        int groups_updated = 0;
    };

    struct DuplicateStats
    {
        int total_groups = 0;
        int total_duplicates = 0;
        int files_with_duplicates = 0;
    };

    struct DuplicateGroupWithMembers
    {
        DuplicateGroupRecord group;
        std::vector<DuplicateMemberRecord> members;
    };

    struct DuplicateGroupsPage
    {
        std::vector<DuplicateGroupWithMembers> groups;
        int total_count = 0;
        int start = 0;
        int end = 0;
        int returned = 0;
    };

    class DuplicateGroupsOps
    {
    public:
        // Table initialization
        static bool ensureTables(DatabaseManager &db);

        /**
         * @brief DISC-1: per-file edge-discovery state on scanned_files.
         *
         * Adds discovery_done to pre-existing databases, creates the pending
         * partial index, and performs the one-time migration off the id
         * watermark (dropping the legacy requeue table and trigger).
         * Idempotent; called from ensureTables(), exposed for tests.
         */
        static bool ensureDiscoveryState(DatabaseManager &db);

        // Group operations
        static int createGroup(DatabaseManager &db,
                               const std::string &mode,
                               int representative_file_id,
                               const std::string &representative_file_path,
                               int64_t representative_file_size,
                               const std::string &representative_created_date,
                               double similarity_threshold);

        static bool updateGroupRepresentative(DatabaseManager &db,
                                              int group_id,
                                              int new_representative_file_id,
                                              const std::string &new_representative_file_path,
                                              int64_t new_representative_file_size,
                                              const std::string &new_representative_created_date,
                                              int member_count);

        static bool deleteGroup(DatabaseManager &db, int group_id);

        static bool deleteGroupsByMode(DatabaseManager &db, const std::string &mode);

        static std::optional<DuplicateGroupRecord> getGroupById(DatabaseManager &db, int group_id);

        static std::vector<DuplicateGroupRecord> getGroupsByMode(DatabaseManager &db, const std::string &mode);

        static std::vector<DuplicateGroupRecord> getGroupsForFile(DatabaseManager &db,
                                                                  int file_id,
                                                                  const std::string &mode);

        // Member operations
        static bool addMember(DatabaseManager &db,
                              int group_id,
                              int file_id,
                              const std::string &file_path,
                              double similarity_score,
                              int64_t file_size,
                              const std::string &created_date,
                              bool is_representative = false);

        static bool updateMemberRepresentativeFlag(DatabaseManager &db,
                                                   int group_id,
                                                   int file_id,
                                                   bool is_representative);

        static bool deleteMember(DatabaseManager &db, int group_id, int file_id);

        static bool removeMember(DatabaseManager &db, int group_id, int file_id);

        static bool updateGroupMemberCount(DatabaseManager &db, int group_id, int member_count);

        // Accurately recalculates the true member count and synchronizes the DB, returning the true count.
        // If the true count is <= 1, it safely purges the orphaned group entirely.
        static int syncGroupMemberCount(DatabaseManager &db, int group_id);

        static std::vector<DuplicateMemberRecord> getMembersByGroup(DatabaseManager &db, int group_id);

        // Statistics
        static int countGroupsByMode(DatabaseManager &db, const std::string &mode);
        
        static int getDuplicateGroupsCountOlderThan(DatabaseManager &db, const std::string &mode, const std::string &date);

        /// 0-based row index of `group_id` in the paginated display order
        /// (newest representative capture date first, id descending within a
        /// date). Returns -1 when the group no longer exists.
        static int getGroupRank(DatabaseManager &db, int group_id);

        static int countMembersByGroup(DatabaseManager &db, int group_id);

        static DuplicateStats getStats(DatabaseManager &db, const std::string &mode);

        // Paginated retrieval with members (for API)
        static DuplicateGroupsPage getGroupsWithMembers(DatabaseManager &db, int start, int limit, const std::string &mode = "");

        // Paths-only bulk-op candidates (no member payloads). Ordering matches
        // the BINNED display order: newest group first, best match first.
        // Non-representative members within `tightness` distance (score <= 0
        // counts as distance 0); groups of >= 2 members only.
        static std::vector<std::string> getEjectCandidatePaths(DatabaseManager &db, double tightness);
        // Every member path of every real (>= 2 member) group; the caller keeps
        // the ejected subset by ejection-root prefix.
        static std::vector<std::string> getAllMemberPaths(DatabaseManager &db);

        // Checkpoint operations
        static bool upsertCheckpoint(DatabaseManager &db,
                                     const std::string &mode,
                                     int last_processed_id,
                                     int files_checked,
                                     int duplicates_found,
                                     int groups_created,
                                     int groups_updated);

        static std::optional<DuplicateCheckpointRecord> getCheckpoint(DatabaseManager &db,
                                                                      const std::string &mode);

        static bool resetCheckpoint(DatabaseManager &db, const std::string &mode);
    };
}
