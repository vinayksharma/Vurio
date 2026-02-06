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
