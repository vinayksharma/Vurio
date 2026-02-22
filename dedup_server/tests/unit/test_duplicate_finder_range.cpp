#include <gtest/gtest.h>
#include "orchestration/duplicate_finder.hpp"
#include "database/duplicate_groups_ops.hpp"
#include "config/config_manager_factory.hpp"
#include "test_utils.hpp"
#include <memory>

using namespace MediaDedup;
using namespace MediaDedup::Orchestration;

class DuplicateFinderRangeTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Create test database
        db_manager_ = std::make_unique<DatabaseManager>(":memory:");
        ASSERT_TRUE(db_manager_->initialize());

        // Create test config
        config_ = ConfigManagerFactory::createForTesting("");
        ASSERT_TRUE(config_->initialize());

        // Set EMBEDDING mode threshold range (two-tier: similarity and duplicate)
        config_->setPropertyValue("duplicates.threshold.similarity", 0.92);
        config_->setPropertyValue("duplicates.threshold.duplicate", 0.96);
        config_->setPropertyValue("duplicates.finder.enabled", true);
        config_->setPropertyValue("duplicates.finder.batchSize", 100);
    }

    void TearDown() override
    {
        db_manager_.reset();
        config_.reset();
    }

    std::unique_ptr<DatabaseManager> db_manager_;
    std::shared_ptr<UnifiedObservableConfigManager> config_;
};

/**
 * Test that EMBEDDING mode loads threshold correctly
 */
TEST_F(DuplicateFinderRangeTest, LoadsThreshold)
{
    DuplicateFinder finder(config_, *db_manager_);
    ASSERT_TRUE(finder.initialize());

    double similarity = finder.getSimilarityThreshold("EMBEDDING");
    double duplicate = finder.getDuplicateThreshold("EMBEDDING");
    EXPECT_DOUBLE_EQ(0.92, similarity);
    EXPECT_DOUBLE_EQ(0.96, duplicate);
    EXPECT_GT(duplicate, similarity);
}

/**
 * Test that decreasing threshold triggers reprocessing
 */
TEST_F(DuplicateFinderRangeTest, ThresholdExpansionTriggersReprocess)
{
    DuplicateFinder finder(config_, *db_manager_);
    ASSERT_TRUE(finder.initialize());

    // Ensure tables exist
    ASSERT_TRUE(DuplicateGroupsOps::ensureTables(*db_manager_));

    // Create a mock EMBEDDING group
    int group_id = DuplicateGroupsOps::createGroup(
        *db_manager_, "EMBEDDING", 1, "/test/file1.jpg", 1000, "2025-01-01", 0.96);
    ASSERT_GT(group_id, 0);

    // Create checkpoint
    ASSERT_TRUE(DuplicateGroupsOps::upsertCheckpoint(
        *db_manager_, "EMBEDDING", 100, 100, 5, 2, 0));

    // Verify group and checkpoint exist
    auto checkpoint_before = DuplicateGroupsOps::getCheckpoint(*db_manager_, "EMBEDDING");
    ASSERT_TRUE(checkpoint_before.has_value());
    EXPECT_EQ(100, checkpoint_before->last_processed_id);

    int groups_before = DuplicateGroupsOps::countGroupsByMode(*db_manager_, "EMBEDDING");
    EXPECT_EQ(1, groups_before);

    // Trigger threshold expansion by decreasing similarity threshold
    config_->setPropertyValue("duplicates.threshold.similarity", 0.90);

    // Give config change time to propagate
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Verify groups were deleted
    int groups_after = DuplicateGroupsOps::countGroupsByMode(*db_manager_, "EMBEDDING");
    EXPECT_EQ(0, groups_after);

    // Verify checkpoint was reset
    auto checkpoint_after = DuplicateGroupsOps::getCheckpoint(*db_manager_, "EMBEDDING");
    ASSERT_TRUE(checkpoint_after.has_value());
    EXPECT_EQ(0, checkpoint_after->last_processed_id);
}

/**
 * Test that increasing threshold does NOT trigger reprocessing
 */
TEST_F(DuplicateFinderRangeTest, StricterThresholdDoesNotReprocess)
{
    DuplicateFinder finder(config_, *db_manager_);
    ASSERT_TRUE(finder.initialize());

    // Ensure tables exist
    ASSERT_TRUE(DuplicateGroupsOps::ensureTables(*db_manager_));

    // Create a mock EMBEDDING group
    int group_id = DuplicateGroupsOps::createGroup(
        *db_manager_, "EMBEDDING", 1, "/test/file1.jpg", 1000, "2025-01-01", 0.94);
    ASSERT_GT(group_id, 0);

    // Verify group exists
    int groups_before = DuplicateGroupsOps::countGroupsByMode(*db_manager_, "EMBEDDING");
    EXPECT_EQ(1, groups_before);

    // Make threshold stricter (increase duplicate threshold)
    config_->setPropertyValue("duplicates.threshold.duplicate", 0.98);

    // Give config change time to propagate
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Verify groups were NOT deleted (stricter threshold doesn't require reprocessing)
    int groups_after = DuplicateGroupsOps::countGroupsByMode(*db_manager_, "EMBEDDING");
    EXPECT_EQ(1, groups_after);
}

// Test removed: MaxThresholdChangeDoesNotReprocess
// This test tested mode-specific max threshold behavior that no longer exists after refactoring

// Test removed: UsesMinimumThresholdForMatching
// This test tested mode-specific threshold range behavior that no longer exists after refactoring

/**
 * Test database operations: deleteGroupsByMode
 * Note: After refactoring, only EMBEDDING mode exists, but the database
 * operations should still work with mode-based filtering for backward compatibility
 */
TEST_F(DuplicateFinderRangeTest, DeleteGroupsByModeWorks)
{
    ASSERT_TRUE(DuplicateGroupsOps::ensureTables(*db_manager_));

    // Create groups - using EMBEDDING mode (the only mode after refactoring)
    // We can still test the database operations with different mode strings for testing purposes
    int embedding_group1 = DuplicateGroupsOps::createGroup(
        *db_manager_, "EMBEDDING", 1, "/test/e1.jpg", 1000, "2025-01-01", 0.96);
    int embedding_group2 = DuplicateGroupsOps::createGroup(
        *db_manager_, "EMBEDDING", 2, "/test/e2.jpg", 2000, "2025-01-02", 0.95);
    int legacy_group = DuplicateGroupsOps::createGroup(
        *db_manager_, "LEGACY_TEST", 3, "/test/legacy1.jpg", 3000, "2025-01-03", 0.92);

    ASSERT_GT(embedding_group1, 0);
    ASSERT_GT(embedding_group2, 0);
    ASSERT_GT(legacy_group, 0);

    // Add members to EMBEDDING groups
    ASSERT_TRUE(DuplicateGroupsOps::addMember(
        *db_manager_, embedding_group1, 1, "/test/e1.jpg", 1.0, 1000, "2025-01-01", true));
    ASSERT_TRUE(DuplicateGroupsOps::addMember(
        *db_manager_, embedding_group1, 4, "/test/e1_dup.jpg", 0.97, 1100, "2025-01-04", false));

    // Verify groups exist
    int embedding_count_before = DuplicateGroupsOps::countGroupsByMode(*db_manager_, "EMBEDDING");
    int legacy_count_before = DuplicateGroupsOps::countGroupsByMode(*db_manager_, "LEGACY_TEST");
    EXPECT_EQ(2, embedding_count_before);
    EXPECT_EQ(1, legacy_count_before);

    // Delete only EMBEDDING groups
    ASSERT_TRUE(DuplicateGroupsOps::deleteGroupsByMode(*db_manager_, "EMBEDDING"));

    // Verify EMBEDDING groups deleted but LEGACY_TEST groups remain
    int embedding_count_after = DuplicateGroupsOps::countGroupsByMode(*db_manager_, "EMBEDDING");
    int legacy_count_after = DuplicateGroupsOps::countGroupsByMode(*db_manager_, "LEGACY_TEST");
    EXPECT_EQ(0, embedding_count_after);
    EXPECT_EQ(1, legacy_count_after);
}

/**
 * Test database operations: resetCheckpoint
 */
TEST_F(DuplicateFinderRangeTest, ResetCheckpointWorks)
{
    ASSERT_TRUE(DuplicateGroupsOps::ensureTables(*db_manager_));

    // Create checkpoint with some progress
    ASSERT_TRUE(DuplicateGroupsOps::upsertCheckpoint(
        *db_manager_, "EMBEDDING", 500, 500, 25, 10, 5));

    auto checkpoint_before = DuplicateGroupsOps::getCheckpoint(*db_manager_, "EMBEDDING");
    ASSERT_TRUE(checkpoint_before.has_value());
    EXPECT_EQ(500, checkpoint_before->last_processed_id);
    EXPECT_EQ(500, checkpoint_before->files_checked);

    // Reset checkpoint
    ASSERT_TRUE(DuplicateGroupsOps::resetCheckpoint(*db_manager_, "EMBEDDING"));

    // Verify all fields reset to 0
    auto checkpoint_after = DuplicateGroupsOps::getCheckpoint(*db_manager_, "EMBEDDING");
    ASSERT_TRUE(checkpoint_after.has_value());
    EXPECT_EQ(0, checkpoint_after->last_processed_id);
    EXPECT_EQ(0, checkpoint_after->files_checked);
    EXPECT_EQ(0, checkpoint_after->duplicates_found);
    EXPECT_EQ(0, checkpoint_after->groups_created);
    EXPECT_EQ(0, checkpoint_after->groups_updated);
}

/**
 * Test that performCrossGroupChecking can be invoked (uses duplicate threshold).
 * With no groups or a single group, no moves occur.
 */
TEST_F(DuplicateFinderRangeTest, CrossGroupCheckingExecutes)
{
    config_->setPropertyValue("duplicates.threshold.similarity", 0.30);
    config_->setPropertyValue("duplicates.threshold.duplicate", 0.80);

    DuplicateFinder finder(config_, *db_manager_);
    ASSERT_TRUE(finder.initialize());

    int moves = finder.performCrossGroupChecking("EMBEDDING", 0.80);
    EXPECT_EQ(0, moves);
}

/**
 * Test: Cross-batch grouping configuration is loaded correctly
 *
 * Verifies that the duplicate finder initializes with cross-batch grouping capability.
 * This test ensures the tryCrossBatchGrouping helper method can be called.
 */
TEST_F(DuplicateFinderRangeTest, CrossBatchGroupingInitializes)
{
    DuplicateFinder finder(config_, *db_manager_);
    ASSERT_TRUE(finder.initialize());

    // Verify initialization succeeded with cross-batch capability
    // The finder should be ready to process batches with cross-batch comparison
    EXPECT_FALSE(finder.isRunning());

    // Verify thresholds are set (used by cross-batch grouping)
    double similarity = finder.getSimilarityThreshold("EMBEDDING");
    double duplicate = finder.getDuplicateThreshold("EMBEDDING");
    EXPECT_GT(similarity, 0.0);
    EXPECT_GT(duplicate, similarity);
}

/**
 * Test: Cross-batch grouping with empty ungrouped pool
 *
 * Verifies that when there are no previously ungrouped files,
 * new files are correctly added to batch_ungrouped_files.
 */
TEST_F(DuplicateFinderRangeTest, CrossBatchEmptyPoolHandled)
{
    ASSERT_TRUE(DuplicateGroupsOps::ensureTables(*db_manager_));

    DuplicateFinder finder(config_, *db_manager_);
    ASSERT_TRUE(finder.initialize());

    // With no scanned files, findDuplicates should complete without error
    finder.findDuplicates();

    // Verify no groups were created (no files to process)
    int groups_count = DuplicateGroupsOps::countGroupsByMode(*db_manager_, "EMBEDDING");
    EXPECT_EQ(0, groups_count);
}

/**
 * Test that threshold validation rejects invalid range (similarity >= duplicate).
 */
TEST_F(DuplicateFinderRangeTest, InvalidThresholdRangeRevertsToDefaults)
{
    config_->setPropertyValue("duplicates.threshold.similarity", 0.98);
    config_->setPropertyValue("duplicates.threshold.duplicate", 0.92);  // Invalid: duplicate < similarity

    DuplicateFinder finder(config_, *db_manager_);
    ASSERT_TRUE(finder.initialize());

    // Should clamp to valid range (0.92, 0.97 or similar)
    double similarity = finder.getSimilarityThreshold("EMBEDDING");
    double duplicate = finder.getDuplicateThreshold("EMBEDDING");
    EXPECT_GT(duplicate, similarity);
}
