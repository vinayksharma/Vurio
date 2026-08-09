/**
 * @file test_duplicates_reset_api.cpp
 * @brief Unit tests for DELETE /api/v1/duplicates/reset endpoint
 *
 * Tests the duplicate detection reset API that clears groups and checkpoints
 * while preserving processed artifacts (scanned files, image artifacts).
 */

#include <gtest/gtest.h>
#include "database/database_manager.hpp"
#include "database/duplicate_groups_ops.hpp"
#include "database/scanned_files_ops.hpp"
#include <filesystem>

using namespace MediaDedup;

class DuplicatesResetAPITest : public ::testing::Test
{
protected:
    std::string test_db_path_;
    std::unique_ptr<DatabaseManager> db_;

    void SetUp() override
    {
        // Create test database in temp directory
        test_db_path_ = "/tmp/test_dup_reset_" + std::to_string(std::time(nullptr)) + ".db";

        // Remove if exists
        std::filesystem::remove(test_db_path_);

        // Initialize database
        db_ = std::make_unique<DatabaseManager>(test_db_path_);
        ASSERT_TRUE(db_->initialize());

        // Ensure tables exist
        ASSERT_TRUE(ScannedFilesOps::ensureTable(*db_));
        ASSERT_TRUE(DuplicateGroupsOps::ensureTables(*db_));
    }

    void TearDown() override
    {
        db_.reset();
        std::filesystem::remove(test_db_path_);
    }

    void createTestGroups(const std::string &mode, int count)
    {
        for (int i = 0; i < count; ++i)
        {
            std::string file_path = "/test/file_" + mode + "_" + std::to_string(i) + ".jpg";
            
            // Insert mock scanned file to satisfy foreign keys
            ScannedFileRow file_row;
            file_row.file_path = file_path;
            file_row.relative_path = file_path;
            file_row.location_key = "test_location";
            file_row.file_name = file_path;
            ScannedFilesOps::upsert(*db_, file_row);
            
            auto inserted = ScannedFilesOps::getByPath(*db_, file_path);
            ASSERT_TRUE(inserted.has_value());
            int file_id = inserted->id;

            int group_id = DuplicateGroupsOps::createGroup(
                *db_, mode, file_id, file_path, 1024, "2025-10-21", 0.95);
            ASSERT_GT(group_id, 0);

            // Add a member
            bool added = DuplicateGroupsOps::addMember(
                *db_, group_id, file_id, file_path, 1.0, 1024, "2025-10-21", true);
            ASSERT_TRUE(added);
        }
    }

    void createTestCheckpoint(const std::string &mode, int last_id, int groups)
    {
        bool created = DuplicateGroupsOps::upsertCheckpoint(
            *db_, mode, last_id, 1000, 500, groups, 10);
        ASSERT_TRUE(created);
    }

    int countAllGroups()
    {
        // After mode refactoring, only EMBEDDING mode exists
        return DuplicateGroupsOps::countGroupsByMode(*db_, "EMBEDDING");
    }
};

TEST_F(DuplicatesResetAPITest, DeleteGroupsByMode_RemovesOnlySpecifiedMode)
{
    // After mode refactoring, only EMBEDDING mode exists
    // Create groups for EMBEDDING and a test legacy mode
    createTestGroups("EMBEDDING", 15);
    createTestGroups("LEGACY_TEST", 3);

    // Verify initial counts
    EXPECT_EQ(DuplicateGroupsOps::countGroupsByMode(*db_, "EMBEDDING"), 15);
    EXPECT_EQ(DuplicateGroupsOps::countGroupsByMode(*db_, "LEGACY_TEST"), 3);

    // Delete only EMBEDDING mode
    bool deleted = DuplicateGroupsOps::deleteGroupsByMode(*db_, "EMBEDDING");
    ASSERT_TRUE(deleted);

    // Verify EMBEDDING deleted, LEGACY_TEST preserved
    EXPECT_EQ(DuplicateGroupsOps::countGroupsByMode(*db_, "EMBEDDING"), 0);
    EXPECT_EQ(DuplicateGroupsOps::countGroupsByMode(*db_, "LEGACY_TEST"), 3);
}

TEST_F(DuplicatesResetAPITest, DeleteGroupsByMode_ClearsGroupsCompletely)
{
    // After mode refactoring, only EMBEDDING mode exists
    // Create groups with members
    createTestGroups("EMBEDDING", 8);

    // Verify groups created
    EXPECT_EQ(DuplicateGroupsOps::countGroupsByMode(*db_, "EMBEDDING"), 8);

    // Delete EMBEDDING groups
    bool deleted = DuplicateGroupsOps::deleteGroupsByMode(*db_, "EMBEDDING");
    ASSERT_TRUE(deleted);

    // Verify EMBEDDING groups deleted
    EXPECT_EQ(DuplicateGroupsOps::countGroupsByMode(*db_, "EMBEDDING"), 0);
}

TEST_F(DuplicatesResetAPITest, ResetCheckpoint_ResetsToZero)
{
    // After mode refactoring, only EMBEDDING mode exists
    // Create checkpoint with progress
    createTestCheckpoint("EMBEDDING", 10000, 150);

    // Verify checkpoint exists with progress
    auto checkpoint_opt = DuplicateGroupsOps::getCheckpoint(*db_, "EMBEDDING");
    ASSERT_TRUE(checkpoint_opt.has_value());
    EXPECT_EQ(checkpoint_opt->last_processed_id, 10000);
    EXPECT_EQ(checkpoint_opt->groups_created, 150);

    // Reset checkpoint
    bool reset = DuplicateGroupsOps::resetCheckpoint(*db_, "EMBEDDING");
    ASSERT_TRUE(reset);

    // Verify checkpoint reset to 0
    checkpoint_opt = DuplicateGroupsOps::getCheckpoint(*db_, "EMBEDDING");
    ASSERT_TRUE(checkpoint_opt.has_value());
    EXPECT_EQ(checkpoint_opt->last_processed_id, 0);
    EXPECT_EQ(checkpoint_opt->files_checked, 0);
    EXPECT_EQ(checkpoint_opt->duplicates_found, 0);
    EXPECT_EQ(checkpoint_opt->groups_created, 0);
    EXPECT_EQ(checkpoint_opt->groups_updated, 0);
}

TEST_F(DuplicatesResetAPITest, ResetAll_ClearsAllModes)
{
    // After mode refactoring, only EMBEDDING mode exists
    // Create groups and checkpoint for EMBEDDING mode
    createTestGroups("EMBEDDING", 15);
    createTestCheckpoint("EMBEDDING", 15000, 15);

    // Verify initial state
    EXPECT_EQ(DuplicateGroupsOps::countGroupsByMode(*db_, "EMBEDDING"), 15);

    // Reset EMBEDDING mode (simulating the API call)
    ASSERT_TRUE(DuplicateGroupsOps::deleteGroupsByMode(*db_, "EMBEDDING"));
    ASSERT_TRUE(DuplicateGroupsOps::resetCheckpoint(*db_, "EMBEDDING"));

    // Verify all groups deleted
    EXPECT_EQ(DuplicateGroupsOps::countGroupsByMode(*db_, "EMBEDDING"), 0);

    // Verify checkpoint reset
    auto checkpoint_embedding = DuplicateGroupsOps::getCheckpoint(*db_, "EMBEDDING");
    ASSERT_TRUE(checkpoint_embedding.has_value());
    EXPECT_EQ(checkpoint_embedding->last_processed_id, 0);
}

// ---------------------------------------------------------------------------
// Paths-only bulk-op candidate queries (frontend 2.2):
// GetEligibleDuplicatePaths / GetEjectedDuplicatePaths back onto these ops.
// ---------------------------------------------------------------------------

class DuplicateBulkPathsTest : public DuplicatesResetAPITest
{
protected:
    int makeScannedFile(const std::string &path)
    {
        ScannedFileRow row;
        row.file_path = path;
        row.relative_path = path;
        row.location_key = "test_location";
        row.file_name = path;
        ScannedFilesOps::upsert(*db_, row);
        auto inserted = ScannedFilesOps::getByPath(*db_, path);
        EXPECT_TRUE(inserted.has_value());
        return inserted ? inserted->id : -1;
    }
};

TEST_F(DuplicateBulkPathsTest, EjectCandidates_FilterByTightnessRepAndMemberCount)
{
    // Real group: representative (1.0) + close member (0.99) + far member (0.90).
    int rep_id = makeScannedFile("/test/rep.jpg");
    int close_id = makeScannedFile("/test/close.jpg");
    int far_id = makeScannedFile("/test/far.jpg");
    int group_id = DuplicateGroupsOps::createGroup(
        *db_, "EMBEDDING", rep_id, "/test/rep.jpg", 1024, "2025-10-21", 0.95);
    ASSERT_GT(group_id, 0);
    ASSERT_TRUE(DuplicateGroupsOps::addMember(*db_, group_id, rep_id, "/test/rep.jpg", 1.0, 1024, "2025-10-21", true));
    ASSERT_TRUE(DuplicateGroupsOps::addMember(*db_, group_id, close_id, "/test/close.jpg", 0.99, 1024, "2025-10-21", false));
    ASSERT_TRUE(DuplicateGroupsOps::addMember(*db_, group_id, far_id, "/test/far.jpg", 0.90, 1024, "2025-10-21", false));
    ASSERT_TRUE(DuplicateGroupsOps::updateGroupMemberCount(*db_, group_id, 3));

    // Degenerate group (member_count stays 1): its member must never appear.
    int lone_id = makeScannedFile("/test/lone.jpg");
    int lone_group = DuplicateGroupsOps::createGroup(
        *db_, "EMBEDDING", lone_id, "/test/lone.jpg", 1024, "2025-10-22", 0.95);
    ASSERT_GT(lone_group, 0);
    ASSERT_TRUE(DuplicateGroupsOps::addMember(*db_, lone_group, lone_id, "/test/lone.jpg", 0.99, 1024, "2025-10-22", false));

    // Tight: only the 0.99 member (distance 0.01); the representative never counts.
    auto tight = DuplicateGroupsOps::getEjectCandidatePaths(*db_, 0.02);
    EXPECT_EQ(tight, std::vector<std::string>({"/test/close.jpg"}));

    // Loose: both non-rep members, best match first.
    auto loose = DuplicateGroupsOps::getEjectCandidatePaths(*db_, 0.2);
    EXPECT_EQ(loose, std::vector<std::string>({"/test/close.jpg", "/test/far.jpg"}));
}

TEST_F(DuplicateBulkPathsTest, EjectCandidates_ZeroScoreCountsAsExactDuplicate)
{
    int rep_id = makeScannedFile("/test/rep0.jpg");
    int exact_id = makeScannedFile("/test/exact0.jpg");
    int group_id = DuplicateGroupsOps::createGroup(
        *db_, "EMBEDDING", rep_id, "/test/rep0.jpg", 1024, "2025-10-21", 0.95);
    ASSERT_GT(group_id, 0);
    ASSERT_TRUE(DuplicateGroupsOps::addMember(*db_, group_id, rep_id, "/test/rep0.jpg", 1.0, 1024, "2025-10-21", true));
    // SHA-identical members are recorded with score 0 — distance 0, always eligible.
    ASSERT_TRUE(DuplicateGroupsOps::addMember(*db_, group_id, exact_id, "/test/exact0.jpg", 0.0, 1024, "2025-10-21", false));
    ASSERT_TRUE(DuplicateGroupsOps::updateGroupMemberCount(*db_, group_id, 2));

    auto strictest = DuplicateGroupsOps::getEjectCandidatePaths(*db_, 0.0);
    EXPECT_EQ(strictest, std::vector<std::string>({"/test/exact0.jpg"}));
}

TEST_F(DuplicateBulkPathsTest, AllMemberPaths_ReturnsEveryMemberOfRealGroups)
{
    int rep_id = makeScannedFile("/test/a.jpg");
    int member_id = makeScannedFile("/test/b.jpg");
    int group_id = DuplicateGroupsOps::createGroup(
        *db_, "EMBEDDING", rep_id, "/test/a.jpg", 1024, "2025-10-21", 0.95);
    ASSERT_GT(group_id, 0);
    ASSERT_TRUE(DuplicateGroupsOps::addMember(*db_, group_id, rep_id, "/test/a.jpg", 1.0, 1024, "2025-10-21", true));
    ASSERT_TRUE(DuplicateGroupsOps::addMember(*db_, group_id, member_id, "/test/b.jpg", 0.97, 1024, "2025-10-21", false));
    ASSERT_TRUE(DuplicateGroupsOps::updateGroupMemberCount(*db_, group_id, 2));

    auto all = DuplicateGroupsOps::getAllMemberPaths(*db_);
    ASSERT_EQ(all.size(), 2u);
    // Representative first only by score ordering (1.0 > 0.97).
    EXPECT_EQ(all[0], "/test/a.jpg");
    EXPECT_EQ(all[1], "/test/b.jpg");
}

// AR-5 pattern: exercise the new gRPC handlers directly, no server needed.
#include "core/grpc_server/duplicates_service.hpp"
#include "database/user_settings_ops.hpp"
#include <chrono>
#include <thread>

TEST_F(DuplicateBulkPathsTest, GrpcHandlers_EligibleAndEjectedPaths)
{
    ASSERT_TRUE(UserSettingsOps::ensureTable(*db_));
    ASSERT_TRUE(UserSettingsOps::upsert(*db_, "ejectionPath:test_location", "/test/bin/ejected_x"));

    // Group: representative + one eligible member + one already in the bin.
    int rep_id = makeScannedFile("/test/h_rep.jpg");
    int cand_id = makeScannedFile("/test/h_cand.jpg");
    int ejected_id = makeScannedFile("/test/bin/ejected_x/h_gone.jpg");
    int group_id = DuplicateGroupsOps::createGroup(
        *db_, "EMBEDDING", rep_id, "/test/h_rep.jpg", 1024, "2025-10-21", 0.95);
    ASSERT_GT(group_id, 0);
    ASSERT_TRUE(DuplicateGroupsOps::addMember(*db_, group_id, rep_id, "/test/h_rep.jpg", 1.0, 1024, "2025-10-21", true));
    ASSERT_TRUE(DuplicateGroupsOps::addMember(*db_, group_id, cand_id, "/test/h_cand.jpg", 0.99, 1024, "2025-10-21", false));
    ASSERT_TRUE(DuplicateGroupsOps::addMember(*db_, group_id, ejected_id, "/test/bin/ejected_x/h_gone.jpg", 0.98, 1024, "2025-10-21", false));
    ASSERT_TRUE(DuplicateGroupsOps::updateGroupMemberCount(*db_, group_id, 3));

    // loadEjectionPaths() memoizes process-wide for 5 s; wait it out so the
    // first handler call below reads THIS test database's settings.
    std::this_thread::sleep_for(std::chrono::milliseconds(5100));

    Core::DuplicatesServiceImpl svc(db_.get());

    grpc::ServerContext ectx;
    vurio::dedup::GetEligibleDuplicatePathsRequest ereq;
    ereq.set_tightness(0.2);
    vurio::dedup::DuplicatePathsResponse eresp;
    ASSERT_TRUE(svc.GetEligibleDuplicatePaths(&ectx, &ereq, &eresp).ok());
    // The representative never counts and the binned member is excluded.
    ASSERT_EQ(eresp.file_paths_size(), 1);
    EXPECT_EQ(eresp.file_paths(0), "/test/h_cand.jpg");

    grpc::ServerContext jctx;
    vurio::dedup::Empty empty;
    vurio::dedup::DuplicatePathsResponse jresp;
    ASSERT_TRUE(svc.GetEjectedDuplicatePaths(&jctx, &empty, &jresp).ok());
    ASSERT_EQ(jresp.file_paths_size(), 1);
    EXPECT_EQ(jresp.file_paths(0), "/test/bin/ejected_x/h_gone.jpg");
}
