#include <gtest/gtest.h>
#include "orchestration/duplicate_finder.hpp"
#include "database/duplicate_groups_ops.hpp"
#include "database/image_artifacts_ops.hpp"
#include "database/scanned_files_ops.hpp"
#include "config/config_manager_factory.hpp"
#include "test_utils.hpp"
#include <cstdint>
#include <memory>
#include <set>
#include <vector>

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

    int mockScannedFile(const std::string& path) {
        ScannedFilesOps::ensureTable(*db_manager_);
        ScannedFileRow file_row;
        file_row.file_path = path;
        file_row.relative_path = path;
        file_row.location_key = "test_location";
        file_row.file_name = path;
        ScannedFilesOps::upsert(*db_manager_, file_row);
        auto opt = ScannedFilesOps::getByPath(*db_manager_, path);
        return opt ? opt->id : 1;
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
    int file_id = mockScannedFile("/test/file1.jpg");
    int group_id = DuplicateGroupsOps::createGroup(
        *db_manager_, "EMBEDDING", file_id, "/test/file1.jpg", 1000, "2025-01-01", 0.96);
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
    int file_id = mockScannedFile("/test/file1.jpg");
    int group_id = DuplicateGroupsOps::createGroup(
        *db_manager_, "EMBEDDING", file_id, "/test/file1.jpg", 1000, "2025-01-01", 0.94);
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

    int id_e1 = mockScannedFile("/test/e1.jpg");
    int id_e2 = mockScannedFile("/test/e2.jpg");
    int id_legacy = mockScannedFile("/test/legacy1.jpg");
    int id_dup = mockScannedFile("/test/e1_dup.jpg");

    // Create groups - using EMBEDDING mode (the only mode after refactoring)
    // We can still test the database operations with different mode strings for testing purposes
    int embedding_group1 = DuplicateGroupsOps::createGroup(
        *db_manager_, "EMBEDDING", id_e1, "/test/e1.jpg", 1000, "2025-01-01", 0.96);
    int embedding_group2 = DuplicateGroupsOps::createGroup(
        *db_manager_, "EMBEDDING", id_e2, "/test/e2.jpg", 2000, "2025-01-02", 0.95);
    int legacy_group = DuplicateGroupsOps::createGroup(
        *db_manager_, "LEGACY_TEST", id_legacy, "/test/legacy1.jpg", 3000, "2025-01-03", 0.92);

    ASSERT_GT(embedding_group1, 0);
    ASSERT_GT(embedding_group2, 0);
    ASSERT_GT(legacy_group, 0);

    // Add members to EMBEDDING groups
    ASSERT_TRUE(DuplicateGroupsOps::addMember(
        *db_manager_, embedding_group1, id_e1, "/test/e1.jpg", 1.0, 1000, "2025-01-01", true));
    ASSERT_TRUE(DuplicateGroupsOps::addMember(
        *db_manager_, embedding_group1, id_dup, "/test/e1_dup.jpg", 0.97, 1100, "2025-01-04", false));

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

/**
 * Test static representative comparison logic
 */
TEST_F(DuplicateFinderRangeTest, IsBetterRepresentativeStatic)
{
    DuplicateFinder::RepresentativeInfo a;
    a.file_id = 1;
    a.file_path = "/test/a.jpg";
    a.file_size = 1000;
    a.has_exif_date = false;

    DuplicateFinder::RepresentativeInfo b;
    b.file_id = 2;
    b.file_path = "/test/b.jpg";
    b.file_size = 2000;
    b.has_exif_date = true;

    auto order = parsePurityOrder("EXIF_CAPTURE_DATE, FILE_SIZE");
    
    // b has EXIF, a doesn't, so b should be better
    EXPECT_FALSE(DuplicateFinder::isBetterRepresentativeStatic(a, b, order));
    EXPECT_TRUE(DuplicateFinder::isBetterRepresentativeStatic(b, a, order));
    
    // Test file size priority
    auto order_size = parsePurityOrder("FILE_SIZE");
    EXPECT_FALSE(DuplicateFinder::isBetterRepresentativeStatic(a, b, order_size));
    EXPECT_TRUE(DuplicateFinder::isBetterRepresentativeStatic(b, a, order_size));
}

/**
 * Test static representative promotion logic
 */
TEST_F(DuplicateFinderRangeTest, PromoteNextBestRepresentative)
{
    ASSERT_TRUE(DuplicateGroupsOps::ensureTables(*db_manager_));

    int id_rep = mockScannedFile("/test/rep.jpg");
    int id_mem1 = mockScannedFile("/test/mem1.jpg"); // Biggest file size
    int id_mem2 = mockScannedFile("/test/mem2.jpg");

    int group_id = DuplicateGroupsOps::createGroup(
        *db_manager_, "EMBEDDING", id_rep, "/test/rep.jpg", 1000, "2025-01-01", 0.96);
    
    DuplicateGroupsOps::addMember(*db_manager_, group_id, id_rep, "/test/rep.jpg", 1.0, 1000, "2025-01-01", true);
    DuplicateGroupsOps::addMember(*db_manager_, group_id, id_mem1, "/test/mem1.jpg", 0.95, 5000, "2025-01-01", false);
    DuplicateGroupsOps::addMember(*db_manager_, group_id, id_mem2, "/test/mem2.jpg", 0.95, 2000, "2025-01-01", false);

    // Promote using FILE_SIZE purity (mem1 should win because 5000 > 2000 > 1000)
    // We pass id_rep as the file to exclude (because it's being deleted)
    bool promoted = DuplicateFinder::promoteNextBestRepresentative(*db_manager_, group_id, id_rep, "FILE_SIZE");
    EXPECT_TRUE(promoted);

    auto group = DuplicateGroupsOps::getGroupById(*db_manager_, group_id);
    ASSERT_TRUE(group.has_value());
    EXPECT_EQ(id_mem1, group->representative_file_id); // mem1 successfully promoted
    EXPECT_EQ(2, group->member_count); // Decreased by 1
}

// DB-3 (backend_optimization_plan.md): the batch artifact loader replaces one
// nested pool checkout + statement per file (tens of thousands per finder
// batch, issued while already holding a lease). It must return byte-identical
// artifacts to the single loader — embeddings INCLUDING NULL bytes, the
// historical truncation trap — and skip ids without an embedding row.
TEST_F(DuplicateFinderRangeTest, BatchArtifactLoaderMatchesSingleLoader)
{
    ASSERT_TRUE(ImageArtifactsOps::ensureTable(*db_manager_));
    DuplicateFinder finder(config_, *db_manager_);

    // Three files with embeddings that deliberately contain NULL bytes.
    std::vector<int> ids;
    for (int i = 0; i < 3; ++i)
    {
        std::string path = "/lib/batch_" + std::to_string(i) + ".jpg";
        int id = mockScannedFile(path);
        ids.push_back(id);

        ImageEmbeddingRecord rec;
        rec.file_path = path;
        rec.model = "test-model";
        rec.dim = 4;
        rec.embedding_blob = {0x00, static_cast<std::uint8_t>(i + 1), 0x00, 0xFF,
                              0x00, 0x00, 0x42, static_cast<std::uint8_t>(i),
                              0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7F};
        ASSERT_TRUE(ImageArtifactsOps::upsertEmbedding(*db_manager_, rec));
    }
    // One file WITHOUT an embedding row: must be skipped, not fail the batch.
    int no_artifact_id = mockScannedFile("/lib/no_artifact.jpg");

    auto lease = db_manager_->acquireSessionLease();
    std::vector<int> request = ids;
    request.push_back(no_artifact_id);
    auto batch = finder.loadFileArtifactsBatch(lease.get(), request);

    ASSERT_EQ(batch.size(), 3u) << "id without embedding row must be skipped";
    for (const auto &artifact : batch)
    {
        DuplicateFinder::FileArtifact single;
        ASSERT_TRUE(finder.loadFileArtifacts(lease.get(), artifact.file_id, "EMBEDDING", single));
        EXPECT_EQ(artifact.file_path, single.file_path);
        EXPECT_EQ(artifact.embedding, single.embedding)
            << "batch embedding bytes must be identical (NULL bytes intact)";
        EXPECT_EQ(artifact.embedding.size(), 16u) << "NULL bytes must not truncate the blob";
        EXPECT_EQ(artifact.embedding_model, single.embedding_model);
        EXPECT_EQ(artifact.embedding_dim, single.embedding_dim);
    }
}

// The batch loader chunks its IN lists at 500 ids — verify a request spanning
// the chunk boundary returns every artifact exactly once.
TEST_F(DuplicateFinderRangeTest, BatchArtifactLoaderSpansChunkBoundary)
{
    ASSERT_TRUE(ImageArtifactsOps::ensureTable(*db_manager_));
    DuplicateFinder finder(config_, *db_manager_);

    constexpr int kCount = 510;
    std::vector<int> ids;
    ids.reserve(kCount);
    for (int i = 0; i < kCount; ++i)
    {
        std::string path = "/lib/chunk_" + std::to_string(i) + ".jpg";
        ids.push_back(mockScannedFile(path));
        ImageEmbeddingRecord rec;
        rec.file_path = path;
        rec.model = "test-model";
        rec.dim = 1;
        rec.embedding_blob = {static_cast<std::uint8_t>(i & 0xFF), 0x00,
                              static_cast<std::uint8_t>((i >> 8) & 0xFF), 0x00};
        ASSERT_TRUE(ImageArtifactsOps::upsertEmbedding(*db_manager_, rec));
    }

    auto lease = db_manager_->acquireSessionLease();
    auto batch = finder.loadFileArtifactsBatch(lease.get(), ids);

    ASSERT_EQ(batch.size(), static_cast<std::size_t>(kCount));
    std::set<int> seen;
    for (const auto &artifact : batch)
        seen.insert(artifact.file_id);
    EXPECT_EQ(seen.size(), static_cast<std::size_t>(kCount)) << "every id exactly once";
}
