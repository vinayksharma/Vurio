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

        // Set QUALITY mode range-based thresholds
        config_->setPropertyValue("duplicates.quality.threshold.min", 0.94);
        config_->setPropertyValue("duplicates.quality.threshold.max", 0.98);
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
 * Test that QUALITY mode loads min/max thresholds correctly
 */
TEST_F(DuplicateFinderRangeTest, LoadsRangeThresholds)
{
    DuplicateFinder finder(config_, *db_manager_);
    ASSERT_TRUE(finder.initialize());

    // Verify threshold is set to minimum (loosest match)
    double threshold = finder.getThreshold("QUALITY");
    EXPECT_DOUBLE_EQ(0.94, threshold);
}

/**
 * Test that decreasing threshold.min triggers reprocessing
 */
TEST_F(DuplicateFinderRangeTest, ThresholdExpansionTriggersReprocess)
{
    DuplicateFinder finder(config_, *db_manager_);
    ASSERT_TRUE(finder.initialize());

    // Ensure tables exist
    ASSERT_TRUE(DuplicateGroupsOps::ensureTables(*db_manager_));

    // Create a mock QUALITY group
    int group_id = DuplicateGroupsOps::createGroup(
        *db_manager_, "QUALITY", 1, "/test/file1.jpg", 1000, "2025-01-01", 0.96);
    ASSERT_GT(group_id, 0);

    // Create checkpoint
    ASSERT_TRUE(DuplicateGroupsOps::upsertCheckpoint(
        *db_manager_, "QUALITY", 100, 100, 5, 2, 0));

    // Verify group and checkpoint exist
    auto checkpoint_before = DuplicateGroupsOps::getCheckpoint(*db_manager_, "QUALITY");
    ASSERT_TRUE(checkpoint_before.has_value());
    EXPECT_EQ(100, checkpoint_before->last_processed_id);

    int groups_before = DuplicateGroupsOps::countGroupsByMode(*db_manager_, "QUALITY");
    EXPECT_EQ(1, groups_before);

    // Trigger threshold expansion by decreasing min
    config_->setPropertyValue("duplicates.quality.threshold.min", 0.92);

    // Give config change time to propagate
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Verify groups were deleted
    int groups_after = DuplicateGroupsOps::countGroupsByMode(*db_manager_, "QUALITY");
    EXPECT_EQ(0, groups_after);

    // Verify checkpoint was reset
    auto checkpoint_after = DuplicateGroupsOps::getCheckpoint(*db_manager_, "QUALITY");
    ASSERT_TRUE(checkpoint_after.has_value());
    EXPECT_EQ(0, checkpoint_after->last_processed_id);
}

/**
 * Test that increasing threshold.min does NOT trigger reprocessing
 */
TEST_F(DuplicateFinderRangeTest, StricterThresholdDoesNotReprocess)
{
    DuplicateFinder finder(config_, *db_manager_);
    ASSERT_TRUE(finder.initialize());

    // Ensure tables exist
    ASSERT_TRUE(DuplicateGroupsOps::ensureTables(*db_manager_));

    // Create a mock QUALITY group
    int group_id = DuplicateGroupsOps::createGroup(
        *db_manager_, "QUALITY", 1, "/test/file1.jpg", 1000, "2025-01-01", 0.94);
    ASSERT_GT(group_id, 0);

    // Verify group exists
    int groups_before = DuplicateGroupsOps::countGroupsByMode(*db_manager_, "QUALITY");
    EXPECT_EQ(1, groups_before);

    // Make threshold stricter (increase min)
    config_->setPropertyValue("duplicates.quality.threshold.min", 0.96);

    // Give config change time to propagate
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Verify groups were NOT deleted (stricter threshold doesn't require reprocessing)
    int groups_after = DuplicateGroupsOps::countGroupsByMode(*db_manager_, "QUALITY");
    EXPECT_EQ(1, groups_after);
}

/**
 * Test that threshold.max updates do not trigger reprocessing
 */
TEST_F(DuplicateFinderRangeTest, MaxThresholdChangeDoesNotReprocess)
{
    DuplicateFinder finder(config_, *db_manager_);
    ASSERT_TRUE(finder.initialize());

    // Ensure tables exist
    ASSERT_TRUE(DuplicateGroupsOps::ensureTables(*db_manager_));

    // Create a mock QUALITY group
    int group_id = DuplicateGroupsOps::createGroup(
        *db_manager_, "QUALITY", 1, "/test/file1.jpg", 1000, "2025-01-01", 0.96);
    ASSERT_GT(group_id, 0);

    // Create checkpoint
    ASSERT_TRUE(DuplicateGroupsOps::upsertCheckpoint(
        *db_manager_, "QUALITY", 50, 50, 3, 1, 0));

    auto checkpoint_before = DuplicateGroupsOps::getCheckpoint(*db_manager_, "QUALITY");
    ASSERT_TRUE(checkpoint_before.has_value());
    int checkpoint_id_before = checkpoint_before->last_processed_id;

    // Change max threshold
    config_->setPropertyValue("duplicates.quality.threshold.max", 0.99);

    // Give config change time to propagate
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Verify checkpoint was NOT reset
    auto checkpoint_after = DuplicateGroupsOps::getCheckpoint(*db_manager_, "QUALITY");
    ASSERT_TRUE(checkpoint_after.has_value());
    EXPECT_EQ(checkpoint_id_before, checkpoint_after->last_processed_id);
}

/**
 * Test that QUALITY mode correctly uses min threshold from range
 */
TEST_F(DuplicateFinderRangeTest, UsesMinimumThresholdForMatching)
{
    // Set range: 0.92 (min) to 0.98 (max)
    config_->setPropertyValue("duplicates.quality.threshold.min", 0.92);
    config_->setPropertyValue("duplicates.quality.threshold.max", 0.98);

    DuplicateFinder finder(config_, *db_manager_);
    ASSERT_TRUE(finder.initialize());

    // Should use minimum threshold (loosest match) for duplicate detection
    double threshold = finder.getThreshold("QUALITY");
    EXPECT_DOUBLE_EQ(0.92, threshold);
}

/**
 * Test database operations: deleteGroupsByMode
 */
TEST_F(DuplicateFinderRangeTest, DeleteGroupsByModeWorks)
{
    ASSERT_TRUE(DuplicateGroupsOps::ensureTables(*db_manager_));

    // Create groups in different modes
    int quality_group1 = DuplicateGroupsOps::createGroup(
        *db_manager_, "QUALITY", 1, "/test/q1.jpg", 1000, "2025-01-01", 0.96);
    int quality_group2 = DuplicateGroupsOps::createGroup(
        *db_manager_, "QUALITY", 2, "/test/q2.jpg", 2000, "2025-01-02", 0.95);
    int fast_group = DuplicateGroupsOps::createGroup(
        *db_manager_, "FAST", 3, "/test/f1.jpg", 3000, "2025-01-03", 0.92);

    ASSERT_GT(quality_group1, 0);
    ASSERT_GT(quality_group2, 0);
    ASSERT_GT(fast_group, 0);

    // Add members to QUALITY groups
    ASSERT_TRUE(DuplicateGroupsOps::addMember(
        *db_manager_, quality_group1, 1, "/test/q1.jpg", 1.0, 1000, "2025-01-01", true));
    ASSERT_TRUE(DuplicateGroupsOps::addMember(
        *db_manager_, quality_group1, 4, "/test/q1_dup.jpg", 0.97, 1100, "2025-01-04", false));

    // Verify groups exist
    int quality_count_before = DuplicateGroupsOps::countGroupsByMode(*db_manager_, "QUALITY");
    int fast_count_before = DuplicateGroupsOps::countGroupsByMode(*db_manager_, "FAST");
    EXPECT_EQ(2, quality_count_before);
    EXPECT_EQ(1, fast_count_before);

    // Delete only QUALITY groups
    ASSERT_TRUE(DuplicateGroupsOps::deleteGroupsByMode(*db_manager_, "QUALITY"));

    // Verify QUALITY groups deleted but FAST groups remain
    int quality_count_after = DuplicateGroupsOps::countGroupsByMode(*db_manager_, "QUALITY");
    int fast_count_after = DuplicateGroupsOps::countGroupsByMode(*db_manager_, "FAST");
    EXPECT_EQ(0, quality_count_after);
    EXPECT_EQ(1, fast_count_after);
}

/**
 * Test database operations: resetCheckpoint
 */
TEST_F(DuplicateFinderRangeTest, ResetCheckpointWorks)
{
    ASSERT_TRUE(DuplicateGroupsOps::ensureTables(*db_manager_));

    // Create checkpoint with some progress
    ASSERT_TRUE(DuplicateGroupsOps::upsertCheckpoint(
        *db_manager_, "QUALITY", 500, 500, 25, 10, 5));

    auto checkpoint_before = DuplicateGroupsOps::getCheckpoint(*db_manager_, "QUALITY");
    ASSERT_TRUE(checkpoint_before.has_value());
    EXPECT_EQ(500, checkpoint_before->last_processed_id);
    EXPECT_EQ(500, checkpoint_before->files_checked);

    // Reset checkpoint
    ASSERT_TRUE(DuplicateGroupsOps::resetCheckpoint(*db_manager_, "QUALITY"));

    // Verify all fields reset to 0
    auto checkpoint_after = DuplicateGroupsOps::getCheckpoint(*db_manager_, "QUALITY");
    ASSERT_TRUE(checkpoint_after.has_value());
    EXPECT_EQ(0, checkpoint_after->last_processed_id);
    EXPECT_EQ(0, checkpoint_after->files_checked);
    EXPECT_EQ(0, checkpoint_after->duplicates_found);
    EXPECT_EQ(0, checkpoint_after->groups_created);
    EXPECT_EQ(0, checkpoint_after->groups_updated);
}
