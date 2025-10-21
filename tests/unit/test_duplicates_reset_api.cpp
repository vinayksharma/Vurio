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
        
        // Ensure duplicate detection tables exist
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
            int group_id = DuplicateGroupsOps::createGroup(
                *db_, mode, i + 1, file_path, 1024, "2025-10-21", 0.95);
            ASSERT_GT(group_id, 0);

            // Add a member
            bool added = DuplicateGroupsOps::addMember(
                *db_, group_id, i + 1, file_path, 1.0, 1024, "2025-10-21", true);
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
        int count = 0;
        count += DuplicateGroupsOps::countGroupsByMode(*db_, "FAST");
        count += DuplicateGroupsOps::countGroupsByMode(*db_, "BALANCED");
        count += DuplicateGroupsOps::countGroupsByMode(*db_, "QUALITY");
        return count;
    }
};

TEST_F(DuplicatesResetAPITest, DeleteGroupsByMode_RemovesOnlySpecifiedMode)
{
    // Create groups for different modes
    createTestGroups("FAST", 5);
    createTestGroups("BALANCED", 3);
    createTestGroups("QUALITY", 7);

    // Verify initial counts
    EXPECT_EQ(DuplicateGroupsOps::countGroupsByMode(*db_, "FAST"), 5);
    EXPECT_EQ(DuplicateGroupsOps::countGroupsByMode(*db_, "BALANCED"), 3);
    EXPECT_EQ(DuplicateGroupsOps::countGroupsByMode(*db_, "QUALITY"), 7);

    // Delete only QUALITY mode
    bool deleted = DuplicateGroupsOps::deleteGroupsByMode(*db_, "QUALITY");
    ASSERT_TRUE(deleted);

    // Verify QUALITY deleted, others preserved
    EXPECT_EQ(DuplicateGroupsOps::countGroupsByMode(*db_, "FAST"), 5);
    EXPECT_EQ(DuplicateGroupsOps::countGroupsByMode(*db_, "BALANCED"), 3);
    EXPECT_EQ(DuplicateGroupsOps::countGroupsByMode(*db_, "QUALITY"), 0);
}

TEST_F(DuplicatesResetAPITest, DeleteGroupsByMode_ClearsGroupsCompletely)
{
    // Create groups with members
    createTestGroups("QUALITY", 5);
    createTestGroups("FAST", 3);

    // Verify groups created
    EXPECT_EQ(DuplicateGroupsOps::countGroupsByMode(*db_, "QUALITY"), 5);
    EXPECT_EQ(DuplicateGroupsOps::countGroupsByMode(*db_, "FAST"), 3);

    // Delete QUALITY groups
    bool deleted = DuplicateGroupsOps::deleteGroupsByMode(*db_, "QUALITY");
    ASSERT_TRUE(deleted);

    // Verify QUALITY groups deleted, FAST preserved
    EXPECT_EQ(DuplicateGroupsOps::countGroupsByMode(*db_, "QUALITY"), 0);
    EXPECT_EQ(DuplicateGroupsOps::countGroupsByMode(*db_, "FAST"), 3);
}

TEST_F(DuplicatesResetAPITest, ResetCheckpoint_ResetsToZero)
{
    // Create checkpoint with progress
    createTestCheckpoint("QUALITY", 10000, 150);

    // Verify checkpoint exists with progress
    auto checkpoint_opt = DuplicateGroupsOps::getCheckpoint(*db_, "QUALITY");
    ASSERT_TRUE(checkpoint_opt.has_value());
    EXPECT_EQ(checkpoint_opt->last_processed_id, 10000);
    EXPECT_EQ(checkpoint_opt->groups_created, 150);

    // Reset checkpoint
    bool reset = DuplicateGroupsOps::resetCheckpoint(*db_, "QUALITY");
    ASSERT_TRUE(reset);

    // Verify checkpoint reset to 0
    checkpoint_opt = DuplicateGroupsOps::getCheckpoint(*db_, "QUALITY");
    ASSERT_TRUE(checkpoint_opt.has_value());
    EXPECT_EQ(checkpoint_opt->last_processed_id, 0);
    EXPECT_EQ(checkpoint_opt->files_checked, 0);
    EXPECT_EQ(checkpoint_opt->duplicates_found, 0);
    EXPECT_EQ(checkpoint_opt->groups_created, 0);
    EXPECT_EQ(checkpoint_opt->groups_updated, 0);
}

TEST_F(DuplicatesResetAPITest, ResetAll_ClearsAllModes)
{
    // Create groups and checkpoints for all modes
    createTestGroups("FAST", 5);
    createTestGroups("BALANCED", 3);
    createTestGroups("QUALITY", 7);
    createTestCheckpoint("FAST", 5000, 5);
    createTestCheckpoint("BALANCED", 3000, 3);
    createTestCheckpoint("QUALITY", 7000, 7);

    // Verify initial state
    EXPECT_EQ(DuplicateGroupsOps::countGroupsByMode(*db_, "FAST"), 5);
    EXPECT_EQ(DuplicateGroupsOps::countGroupsByMode(*db_, "BALANCED"), 3);
    EXPECT_EQ(DuplicateGroupsOps::countGroupsByMode(*db_, "QUALITY"), 7);

    // Reset all modes (simulating the API call)
    std::vector<std::string> modes = {"FAST", "BALANCED", "QUALITY"};
    for (const auto &mode : modes)
    {
        ASSERT_TRUE(DuplicateGroupsOps::deleteGroupsByMode(*db_, mode));
        ASSERT_TRUE(DuplicateGroupsOps::resetCheckpoint(*db_, mode));
    }

    // Verify all groups deleted
    EXPECT_EQ(DuplicateGroupsOps::countGroupsByMode(*db_, "FAST"), 0);
    EXPECT_EQ(DuplicateGroupsOps::countGroupsByMode(*db_, "BALANCED"), 0);
    EXPECT_EQ(DuplicateGroupsOps::countGroupsByMode(*db_, "QUALITY"), 0);

    // Verify all checkpoints reset
    auto checkpoint_fast = DuplicateGroupsOps::getCheckpoint(*db_, "FAST");
    auto checkpoint_balanced = DuplicateGroupsOps::getCheckpoint(*db_, "BALANCED");
    auto checkpoint_quality = DuplicateGroupsOps::getCheckpoint(*db_, "QUALITY");

    ASSERT_TRUE(checkpoint_fast.has_value());
    ASSERT_TRUE(checkpoint_balanced.has_value());
    ASSERT_TRUE(checkpoint_quality.has_value());

    EXPECT_EQ(checkpoint_fast->last_processed_id, 0);
    EXPECT_EQ(checkpoint_balanced->last_processed_id, 0);
    EXPECT_EQ(checkpoint_quality->last_processed_id, 0);
}

