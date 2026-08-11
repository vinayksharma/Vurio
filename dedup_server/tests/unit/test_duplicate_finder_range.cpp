#include <gtest/gtest.h>
#include "orchestration/duplicate_finder.hpp"
#include <Poco/ConsoleChannel.h>
#include <Poco/Logger.h>
#include "database/duplicate_groups_ops.hpp"
#include "database/image_artifacts_ops.hpp"
#include "database/scanned_files_ops.hpp"
#include "database/vector_index.hpp"
#include "config/config_manager_factory.hpp"
#include "test_utils.hpp"
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
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
        // File-backed test database: a ":memory:" DatabaseManager is broken
        // under the connection pool (each pooled connection is its own empty
        // DB) — tests only ever passed by connection-reuse luck.
        fixture_db_path_ = ::MediaDedup::Test::TestUtils::generateTempFilePath("test_dupfinder_range", "db");
        db_manager_ = std::make_unique<DatabaseManager>(fixture_db_path_);
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
        if (!fixture_db_path_.empty())
        {
            ::MediaDedup::Test::TestUtils::deleteFile(fixture_db_path_);
            ::MediaDedup::Test::TestUtils::deleteFile(fixture_db_path_ + "-wal");
            ::MediaDedup::Test::TestUtils::deleteFile(fixture_db_path_ + "-shm");
        }
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

    std::string fixture_db_path_;
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
 * DF-3: decreasing the similarity threshold (within the edge floor) must NOT
 * wipe groups or reset the checkpoint anymore — it only schedules a re-cluster
 * from the persisted edge list, picked up by the next finder run.
 */
TEST_F(DuplicateFinderRangeTest, ThresholdDecreaseSchedulesReclusterWithoutWipe)
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

    // Decrease similarity threshold, staying above the edge floor (0.80)
    config_->setPropertyValue("duplicates.threshold.similarity", 0.90);

    // Give config change time to propagate
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Groups must survive the slider move
    int groups_after = DuplicateGroupsOps::countGroupsByMode(*db_manager_, "EMBEDDING");
    EXPECT_EQ(1, groups_after) << "threshold change must not wipe groups";

    // Checkpoint must be untouched (no re-discovery needed)
    auto checkpoint_after = DuplicateGroupsOps::getCheckpoint(*db_manager_, "EMBEDDING");
    ASSERT_TRUE(checkpoint_after.has_value());
    EXPECT_EQ(100, checkpoint_after->last_processed_id) << "checkpoint must not reset";
}

/**
 * DF-3: dropping the similarity threshold BELOW the edge discovery floor is
 * the one case where recorded edges cannot answer — legacy full reset applies
 * (groups wiped, checkpoint reset, edges cleared for re-discovery).
 */
TEST_F(DuplicateFinderRangeTest, ThresholdBelowEdgeFloorTriggersFullReset)
{
    DuplicateFinder finder(config_, *db_manager_);
    ASSERT_TRUE(finder.initialize());

    ASSERT_TRUE(DuplicateGroupsOps::ensureTables(*db_manager_));

    int file_id = mockScannedFile("/test/file1.jpg");
    int group_id = DuplicateGroupsOps::createGroup(
        *db_manager_, "EMBEDDING", file_id, "/test/file1.jpg", 1000, "2025-01-01", 0.96);
    ASSERT_GT(group_id, 0);
    ASSERT_TRUE(DuplicateGroupsOps::upsertCheckpoint(
        *db_manager_, "EMBEDDING", 100, 100, 5, 2, 0));

    // 0.75 < edge floor 0.80: recorded edges are insufficient
    config_->setPropertyValue("duplicates.threshold.similarity", 0.75);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    EXPECT_EQ(0, DuplicateGroupsOps::countGroupsByMode(*db_manager_, "EMBEDDING"))
        << "below-floor threshold requires full re-discovery";
    auto checkpoint_after = DuplicateGroupsOps::getCheckpoint(*db_manager_, "EMBEDDING");
    ASSERT_TRUE(checkpoint_after.has_value());
    EXPECT_EQ(0, checkpoint_after->last_processed_id);
}

/**
 * DF-3 migration: a pre-edge-list install (advanced watermark, groups, but an
 * EMPTY duplicate_edges table) must get its checkpoint reset at initialize so
 * discovery repopulates the edges — while the existing groups stay visible.
 */
TEST_F(DuplicateFinderRangeTest, EmptyEdgeTableBehindWatermarkResetsCheckpoint)
{
    ASSERT_TRUE(DuplicateGroupsOps::ensureTables(*db_manager_));

    int file_id = mockScannedFile("/test/legacy_rep.jpg");
    int group_id = DuplicateGroupsOps::createGroup(
        *db_manager_, "EMBEDDING", file_id, "/test/legacy_rep.jpg", 1000, "2025-01-01", 0.96);
    ASSERT_GT(group_id, 0);
    ASSERT_TRUE(DuplicateGroupsOps::upsertCheckpoint(
        *db_manager_, "EMBEDDING", 100, 100, 5, 2, 0));

    DuplicateFinder finder(config_, *db_manager_);
    ASSERT_TRUE(finder.initialize());

    auto checkpoint = DuplicateGroupsOps::getCheckpoint(*db_manager_, "EMBEDDING");
    ASSERT_TRUE(checkpoint.has_value());
    EXPECT_EQ(0, checkpoint->last_processed_id)
        << "watermark with no edges must reset for one-time re-discovery";
    EXPECT_EQ(1, DuplicateGroupsOps::countGroupsByMode(*db_manager_, "EMBEDDING"))
        << "existing groups must stay visible through the migration";
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

// DB-6: BINNED group pagination must be a covering SEARCH on the composite
// (mode, representative_created_date DESC) index — the legacy single-column
// mode index forced an index scan + sort (temp B-tree) per page.
TEST_F(DuplicateFinderRangeTest, GroupPaginationUsesCompositeModeDateIndex)
{
    ASSERT_TRUE(DuplicateGroupsOps::ensureTables(*db_manager_));

    auto lease = db_manager_->acquireSessionLease();
    Poco::Data::Session &sess = lease.get();

    // The legacy index must be gone after ensureTables.
    {
        int legacy = -1;
        Poco::Data::Statement q(sess);
        q << "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name='idx_duplicate_groups_mode'",
            Poco::Data::Keywords::into(legacy), Poco::Data::Keywords::now;
        EXPECT_EQ(legacy, 0) << "legacy idx_duplicate_groups_mode must be dropped";
    }

    std::vector<int> ids, parents, unused;
    std::vector<std::string> details;
    Poco::Data::Statement stmt(sess);
    stmt << "EXPLAIN QUERY PLAN SELECT id FROM duplicate_groups WHERE mode='EMBEDDING' "
            "ORDER BY representative_created_date DESC LIMIT 30 OFFSET 0",
        Poco::Data::Keywords::into(ids), Poco::Data::Keywords::into(parents),
        Poco::Data::Keywords::into(unused), Poco::Data::Keywords::into(details),
        Poco::Data::Keywords::now;
    std::string plan;
    for (const auto &d : details)
        plan += d + "\n";
    EXPECT_NE(plan.find("idx_duplicate_groups_mode_rep_date"), std::string::npos) << plan;
    EXPECT_EQ(plan.find("TEMP B-TREE"), std::string::npos) << "pagination must not sort. Plan:\n" << plan;
}

// DB-7: getGroupsWithMembers fetches every page member in ONE set-based query
// instead of one statement per group (N+1). Members must land on the right
// groups, similarity-sorted, with the representative included.
TEST_F(DuplicateFinderRangeTest, GroupsWithMembersBatchedFetch)
{
    ASSERT_TRUE(DuplicateGroupsOps::ensureTables(*db_manager_));

    int rep1 = mockScannedFile("/g/rep1.jpg");
    int mem1 = mockScannedFile("/g/mem1.jpg");
    int rep2 = mockScannedFile("/g/rep2.jpg");
    int mem2a = mockScannedFile("/g/mem2a.jpg");
    int mem2b = mockScannedFile("/g/mem2b.jpg");

    int g1 = DuplicateGroupsOps::createGroup(*db_manager_, "EMBEDDING", rep1, "/g/rep1.jpg", 100, "2025-01-01", 0.96);
    int g2 = DuplicateGroupsOps::createGroup(*db_manager_, "EMBEDDING", rep2, "/g/rep2.jpg", 200, "2025-01-02", 0.96);
    ASSERT_GT(g1, 0);
    ASSERT_GT(g2, 0);

    ASSERT_TRUE(DuplicateGroupsOps::addMember(*db_manager_, g1, rep1, "/g/rep1.jpg", 1.0, 100, "2025-01-01", true));
    ASSERT_TRUE(DuplicateGroupsOps::addMember(*db_manager_, g1, mem1, "/g/mem1.jpg", 0.97, 110, "2025-01-01", false));
    ASSERT_TRUE(DuplicateGroupsOps::addMember(*db_manager_, g2, rep2, "/g/rep2.jpg", 1.0, 200, "2025-01-02", true));
    ASSERT_TRUE(DuplicateGroupsOps::addMember(*db_manager_, g2, mem2a, "/g/mem2a.jpg", 0.93, 210, "2025-01-02", false));
    ASSERT_TRUE(DuplicateGroupsOps::addMember(*db_manager_, g2, mem2b, "/g/mem2b.jpg", 0.98, 220, "2025-01-02", false));

    auto page = DuplicateGroupsOps::getGroupsWithMembers(*db_manager_, 0, 10, "EMBEDDING");
    ASSERT_EQ(page.groups.size(), 2u);

    for (const auto &gwm : page.groups)
    {
        if (gwm.group.id == g1)
        {
            ASSERT_EQ(gwm.members.size(), 2u);
            EXPECT_TRUE(gwm.members[0].is_representative) << "similarity DESC puts the rep (1.0) first";
            EXPECT_EQ(gwm.members[1].file_id, mem1);
        }
        else
        {
            ASSERT_EQ(gwm.group.id, g2);
            ASSERT_EQ(gwm.members.size(), 3u);
            // similarity DESC within the group: 1.0, 0.98, 0.93
            EXPECT_EQ(gwm.members[0].file_id, rep2);
            EXPECT_EQ(gwm.members[1].file_id, mem2b);
            EXPECT_EQ(gwm.members[2].file_id, mem2a);
        }
    }
}

// DF-1 end-to-end: a full findDuplicates() run over fabricated embeddings.
// The two near-identical embeddings must group via the arena fast path
// (normalized dot product), the distinct one must stay ungrouped, and the
// checkpoint must advance. Runs the REAL pipeline: run-state load, batch
// preload into the arena, local-worker fan-out (CC-3), group creation.
TEST_F(DuplicateFinderRangeTest, FindDuplicatesGroupsSimilarEmbeddingsEndToEnd)
{
    // A full findDuplicates run leases many pooled sessions. The fixture's
    // ":memory:" DB gives every pooled connection its OWN empty database, so
    // this end-to-end test uses a file-backed DB instead.
    std::string db_path = (std::filesystem::temp_directory_path() / "df1_end_to_end.sqlite").string();
    std::filesystem::remove(db_path);
    db_manager_ = std::make_unique<DatabaseManager>(db_path);
    ASSERT_TRUE(db_manager_->initialize());

    ASSERT_TRUE(ScannedFilesOps::ensureTable(*db_manager_));
    ASSERT_TRUE(DuplicateGroupsOps::ensureTables(*db_manager_));
    ASSERT_TRUE(ImageArtifactsOps::ensureTable(*db_manager_));
    config_->setPropertyValue("duplicates.finder.enabled", true);

    auto addProcessedFile = [&](const std::string &path, const std::vector<float> &vec) {
        ScannedFileRow row;
        row.file_path = path;
        row.relative_path = path;
        row.file_name = path;
        row.location_key = "test_location";
        row.file_metadata = "{\"sizeBytes\": 1000}";
        row.processed = 2; // finder only considers fully processed files
        EXPECT_TRUE(ScannedFilesOps::upsert(*db_manager_, row));
        auto stored = ScannedFilesOps::getByPath(*db_manager_, path);

        ImageEmbeddingRecord rec;
        rec.file_path = path;
        rec.model = "test-model";
        rec.dim = static_cast<int>(vec.size());
        rec.embedding_blob.resize(vec.size() * sizeof(float));
        std::memcpy(rec.embedding_blob.data(), vec.data(), rec.embedding_blob.size());
        EXPECT_TRUE(ImageArtifactsOps::upsertEmbedding(*db_manager_, rec));
        return stored ? stored->id : 0;
    };

    // 8-dim embeddings, deliberately UNNORMALIZED (magnitudes 2.0 / 3.0) —
    // the arena must normalize at load for the dot product to be a cosine.
    std::vector<float> a  = {2.0f, 0.02f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    std::vector<float> a2 = {3.0f, 0.0f, 0.06f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}; // cos(a,a2) ~ 0.9995
    std::vector<float> b  = {0.0f, 2.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};  // cos(a,b) ~ 0.01

    int id_a  = addProcessedFile("/df1/a.jpg", a);
    int id_a2 = addProcessedFile("/df1/a2.jpg", a2);
    int id_b  = addProcessedFile("/df1/b.jpg", b);
    ASSERT_GT(id_a, 0);
    ASSERT_GT(id_a2, 0);
    ASSERT_GT(id_b, 0);

    DuplicateFinder finder(config_, *db_manager_);
    ASSERT_TRUE(finder.initialize());
    finder.findDuplicates();

    EXPECT_EQ(DuplicateGroupsOps::countGroupsByMode(*db_manager_, "EMBEDDING"), 1)
        << "exactly one group: the two near-identical embeddings";

    auto page = DuplicateGroupsOps::getGroupsWithMembers(*db_manager_, 0, 10, "EMBEDDING");
    ASSERT_EQ(page.groups.size(), 1u);
    ASSERT_EQ(page.groups[0].members.size(), 2u);
    std::set<int> member_ids;
    for (const auto &m : page.groups[0].members)
        member_ids.insert(m.file_id);
    EXPECT_TRUE(member_ids.count(id_a));
    EXPECT_TRUE(member_ids.count(id_a2));
    EXPECT_FALSE(member_ids.count(id_b)) << "the dissimilar file must stay ungrouped";

    auto checkpoint = DuplicateGroupsOps::getCheckpoint(*db_manager_, "EMBEDDING");
    ASSERT_TRUE(checkpoint.has_value());
    EXPECT_GE(checkpoint->last_processed_id, id_b) << "checkpoint must advance past the batch";

    db_manager_.reset();
    std::filesystem::remove(db_path);
}

// DF-2 end-to-end: the same grouping scenario, but with the sqlite-vec index
// ENABLED — candidates come from the KNN query (distance-filtered, no silent
// full-scan fallback). Results must be identical to the linear-scan run.
// Skips when the vec0 extension is not present.
TEST_F(DuplicateFinderRangeTest, FindDuplicatesGroupsViaKnnIndexEndToEnd)
{
    std::string ext;
    for (const char *p : {"extensions/vec0.dylib", "../extensions/vec0.dylib",
                          "../../dedup_server/extensions/vec0.dylib"})
        if (std::filesystem::exists(p)) { ext = std::filesystem::absolute(p).string(); break; }
    if (ext.empty())
        GTEST_SKIP() << "vec0 extension not found";

    auto ensureProp = [&](const char *key, auto value) {
        if (!config_->hasProperty(key))
            config_->createProperty(key, value, "test");
        config_->setPropertyValue(key, value);
    };
    ensureProp("database.vector.enabled", true);
    ensureProp("database.vector.extensionPath", ext);
    ensureProp("media.image.embeddingDim", 8);
    config_->setPropertyValue("duplicates.finder.enabled", true);

    std::string db_path = (std::filesystem::temp_directory_path() / "df2_knn_end_to_end.sqlite").string();
    std::filesystem::remove(db_path);
    // Config-carrying DatabaseManager: initializes AND reconciles the index.
    db_manager_ = std::make_unique<DatabaseManager>(db_path, config_);
    ASSERT_TRUE(db_manager_->initialize());
    ASSERT_NE(db_manager_->getVectorIndex(), nullptr) << "vector index must be active";
    ASSERT_TRUE(db_manager_->getVectorIndex()->isReady());

    ASSERT_TRUE(ScannedFilesOps::ensureTable(*db_manager_));
    ASSERT_TRUE(DuplicateGroupsOps::ensureTables(*db_manager_));
    ASSERT_TRUE(ImageArtifactsOps::ensureTable(*db_manager_));

    auto addProcessedFile = [&](const std::string &path, const std::vector<float> &vec) {
        ScannedFileRow row;
        row.file_path = path;
        row.relative_path = path;
        row.file_name = path;
        row.location_key = "test_location";
        row.file_metadata = "{\"sizeBytes\": 1000}";
        row.processed = 2;
        EXPECT_TRUE(ScannedFilesOps::upsert(*db_manager_, row));
        auto stored = ScannedFilesOps::getByPath(*db_manager_, path);

        ImageEmbeddingRecord rec;
        rec.file_path = path;
        rec.model = "test-model";
        rec.dim = static_cast<int>(vec.size());
        rec.embedding_blob.resize(vec.size() * sizeof(float));
        std::memcpy(rec.embedding_blob.data(), vec.data(), rec.embedding_blob.size());
        EXPECT_TRUE(ImageArtifactsOps::upsertEmbedding(*db_manager_, rec)); // also feeds the index
        return stored ? stored->id : 0;
    };

    std::vector<float> a  = {2.0f, 0.02f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    std::vector<float> a2 = {3.0f, 0.0f, 0.06f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    std::vector<float> b  = {0.0f, 2.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

    int id_a  = addProcessedFile("/df2/a.jpg", a);
    int id_a2 = addProcessedFile("/df2/a2.jpg", a2);
    int id_b  = addProcessedFile("/df2/b.jpg", b);
    ASSERT_GT(id_a, 0);

    DuplicateFinder finder(config_, *db_manager_);
    ASSERT_TRUE(finder.initialize());
    finder.findDuplicates();

    EXPECT_EQ(DuplicateGroupsOps::countGroupsByMode(*db_manager_, "EMBEDDING"), 1);
    auto page = DuplicateGroupsOps::getGroupsWithMembers(*db_manager_, 0, 10, "EMBEDDING");
    ASSERT_EQ(page.groups.size(), 1u);
    ASSERT_EQ(page.groups[0].members.size(), 2u);
    std::set<int> member_ids;
    for (const auto &m : page.groups[0].members)
        member_ids.insert(m.file_id);
    EXPECT_TRUE(member_ids.count(id_a));
    EXPECT_TRUE(member_ids.count(id_a2));
    EXPECT_FALSE(member_ids.count(id_b));

    db_manager_.reset();
    std::filesystem::remove(db_path);
}

// DF-3 end-to-end: moving the tightness slider re-clusters from the persisted
// edge list — no group wipe, no checkpoint reset, no embedding re-comparison.
// Three files: a and a2 are near-identical (cos ~0.9997); m sits at ~0.93 to
// both. At similarity 0.95 the group is {a, a2}; edges to m are recorded at
// discovery time (>= 0.80 floor) but below the grouping bar. Sliding the
// threshold to 0.92 must grow the SAME group to {a, a2, m} purely from edges.
TEST_F(DuplicateFinderRangeTest, SliderReclusterFromEdgesEndToEnd)
{
    std::string db_path = (std::filesystem::temp_directory_path() / "df3_slider_recluster.sqlite").string();
    std::filesystem::remove(db_path);
    db_manager_ = std::make_unique<DatabaseManager>(db_path);
    ASSERT_TRUE(db_manager_->initialize());

    ASSERT_TRUE(ScannedFilesOps::ensureTable(*db_manager_));
    ASSERT_TRUE(DuplicateGroupsOps::ensureTables(*db_manager_));
    ASSERT_TRUE(ImageArtifactsOps::ensureTable(*db_manager_));
    config_->setPropertyValue("duplicates.finder.enabled", true);
    config_->setPropertyValue("duplicates.threshold.similarity", 0.95);
    config_->setPropertyValue("duplicates.threshold.duplicate", 0.97);

    auto addProcessedFile = [&](const std::string &path, const std::vector<float> &vec) {
        ScannedFileRow row;
        row.file_path = path;
        row.relative_path = path;
        row.file_name = path;
        row.location_key = "test_location";
        row.file_metadata = "{\"sizeBytes\": 1000}";
        row.processed = 2;
        EXPECT_TRUE(ScannedFilesOps::upsert(*db_manager_, row));
        auto stored = ScannedFilesOps::getByPath(*db_manager_, path);

        ImageEmbeddingRecord rec;
        rec.file_path = path;
        rec.model = "test-model";
        rec.dim = static_cast<int>(vec.size());
        rec.embedding_blob.resize(vec.size() * sizeof(float));
        std::memcpy(rec.embedding_blob.data(), vec.data(), rec.embedding_blob.size());
        EXPECT_TRUE(ImageArtifactsOps::upsertEmbedding(*db_manager_, rec));
        return stored ? stored->id : 0;
    };

    // cos(a, a2) ~ 0.9997; cos(m, a) = cos(m, a2) ~ 0.93 (unit m by design)
    std::vector<float> a  = {2.0f, 0.02f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    std::vector<float> a2 = {3.0f, 0.0f, 0.06f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    std::vector<float> m  = {0.93f, 0.0f, 0.0f, 0.36756f, 0.0f, 0.0f, 0.0f, 0.0f};

    int id_a  = addProcessedFile("/df3/a.jpg", a);
    int id_a2 = addProcessedFile("/df3/a2.jpg", a2);
    int id_m  = addProcessedFile("/df3/m.jpg", m);
    ASSERT_GT(id_a, 0);
    ASSERT_GT(id_a2, 0);
    ASSERT_GT(id_m, 0);

    DuplicateFinder finder(config_, *db_manager_);
    ASSERT_TRUE(finder.initialize());
    finder.findDuplicates();

    // Tight threshold: one group of the two near-identical files.
    auto page1 = DuplicateGroupsOps::getGroupsWithMembers(*db_manager_, 0, 10, "EMBEDDING");
    ASSERT_EQ(page1.groups.size(), 1u);
    ASSERT_EQ(page1.groups[0].members.size(), 2u);
    const int original_group_id = page1.groups[0].group.id;

    // All three pairs cleared the discovery floor, so all edges are on disk.
    {
        auto lease = db_manager_->acquireSessionLease();
        int edge_count = 0;
        Poco::Data::Statement q(lease.get());
        q << "SELECT COUNT(*) FROM duplicate_edges",
            Poco::Data::Keywords::into(edge_count), Poco::Data::Keywords::now;
        EXPECT_EQ(edge_count, 3) << "a-a2, a-m, a2-m all >= edge floor";
    }
    auto checkpoint_before = DuplicateGroupsOps::getCheckpoint(*db_manager_, "EMBEDDING");
    ASSERT_TRUE(checkpoint_before.has_value());

    // Slide the threshold looser. Groups must survive the config change...
    config_->setPropertyValue("duplicates.threshold.similarity", 0.92);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_EQ(DuplicateGroupsOps::countGroupsByMode(*db_manager_, "EMBEDDING"), 1)
        << "slider must not wipe groups";

    // ...and the next finder run re-clusters from edges: same group, now 3 members.
    finder.findDuplicates();

    auto page2 = DuplicateGroupsOps::getGroupsWithMembers(*db_manager_, 0, 10, "EMBEDDING");
    ASSERT_EQ(page2.groups.size(), 1u);
    EXPECT_EQ(page2.groups[0].group.id, original_group_id) << "group id must be stable across re-cluster";
    ASSERT_EQ(page2.groups[0].members.size(), 3u);
    std::set<int> member_ids;
    for (const auto &memb : page2.groups[0].members)
        member_ids.insert(memb.file_id);
    EXPECT_TRUE(member_ids.count(id_a));
    EXPECT_TRUE(member_ids.count(id_a2));
    EXPECT_TRUE(member_ids.count(id_m)) << "borderline file joins at the looser threshold";

    auto checkpoint_after = DuplicateGroupsOps::getCheckpoint(*db_manager_, "EMBEDDING");
    ASSERT_TRUE(checkpoint_after.has_value());
    EXPECT_EQ(checkpoint_after->last_processed_id, checkpoint_before->last_processed_id)
        << "re-cluster must not re-discover: watermark untouched";

    db_manager_.reset();
    std::filesystem::remove(db_path);
}

// A re-cluster rewrote every group and every member row on every run, holding
// SQLite's single write lock for ~8-10s over ~100k statements while a handful
// of groups had actually changed. Everything else in the process that writes —
// most visibly the thumbnail and preview cache upserts — blocked out and
// failed. Unchanged groups must now survive a run untouched.
TEST_F(DuplicateFinderRangeTest, ReclusterOnlyWritesGroupsThatChanged)
{
    std::string db_path = (std::filesystem::temp_directory_path() / "df_recluster_diff.sqlite").string();
    std::filesystem::remove(db_path);
    db_manager_ = std::make_unique<DatabaseManager>(db_path);
    ASSERT_TRUE(db_manager_->initialize());

    ASSERT_TRUE(ScannedFilesOps::ensureTable(*db_manager_));
    ASSERT_TRUE(DuplicateGroupsOps::ensureTables(*db_manager_));
    ASSERT_TRUE(ImageArtifactsOps::ensureTable(*db_manager_));
    config_->setPropertyValue("duplicates.finder.enabled", true);
    config_->setPropertyValue("duplicates.threshold.similarity", 0.95);
    config_->setPropertyValue("duplicates.threshold.duplicate", 0.97);

    auto addProcessedFile = [&](const std::string &path, const std::vector<float> &vec) {
        ScannedFileRow row;
        row.file_path = path;
        row.relative_path = path;
        row.file_name = path;
        row.location_key = "test_location";
        row.file_metadata = "{\"sizeBytes\": 1000}";
        row.processed = 2;
        EXPECT_TRUE(ScannedFilesOps::upsert(*db_manager_, row));

        ImageEmbeddingRecord rec;
        rec.file_path = path;
        rec.model = "test-model";
        rec.dim = static_cast<int>(vec.size());
        rec.embedding_blob.resize(vec.size() * sizeof(float));
        std::memcpy(rec.embedding_blob.data(), vec.data(), rec.embedding_blob.size());
        EXPECT_TRUE(ImageArtifactsOps::upsertEmbedding(*db_manager_, rec));
    };

    // Two near-identical pairs, orthogonal to each other: clustering the
    // second pair cannot disturb the first.
    addProcessedFile("/diff/a.jpg",  {2.0f, 0.02f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f});
    addProcessedFile("/diff/a2.jpg", {3.0f, 0.0f, 0.06f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f});

    DuplicateFinder finder(config_, *db_manager_);
    ASSERT_TRUE(finder.initialize());
    finder.findDuplicates();

    auto first = DuplicateGroupsOps::getGroupsWithMembers(*db_manager_, 0, 10, "EMBEDDING");
    ASSERT_EQ(first.groups.size(), 1u);
    int stable_group_id = first.groups[0].group.id;

    // Stamp a sentinel a rewrite cannot preserve: the group UPDATE sets
    // updated_at=CURRENT_TIMESTAMP, so a back-dated value surviving proves no
    // UPDATE touched this row. It has to stay a parseable timestamp — the
    // pagination reader parses the column and fails the whole page otherwise.
    const std::string kSentinel = "1999-01-01 00:00:00";
    {
        auto lease = db_manager_->acquireSessionLease();
        std::string sentinel = kSentinel;
        Poco::Data::Statement stamp(lease.get());
        stamp << "UPDATE duplicate_groups SET updated_at = ? WHERE id = ?",
            Poco::Data::Keywords::use(sentinel), Poco::Data::Keywords::use(stable_group_id),
            Poco::Data::Keywords::now;
    }

    // A second, unrelated pair arrives — the steady-state import case.
    addProcessedFile("/diff/b.jpg",  {0.0f, 0.0f, 0.0f, 0.0f, 2.0f, 0.02f, 0.0f, 0.0f});
    addProcessedFile("/diff/b2.jpg", {0.0f, 0.0f, 0.0f, 0.0f, 3.0f, 0.0f, 0.06f, 0.0f});
    finder.findDuplicates();

    auto second = DuplicateGroupsOps::getGroupsWithMembers(*db_manager_, 0, 10, "EMBEDDING");
    ASSERT_EQ(second.groups.size(), 2u) << "the new pair must form its own group";

    std::string stamp_after;
    {
        auto lease = db_manager_->acquireSessionLease();
        Poco::Data::Statement q(lease.get());
        q << "SELECT updated_at FROM duplicate_groups WHERE id = ?",
            Poco::Data::Keywords::into(stamp_after), Poco::Data::Keywords::use(stable_group_id),
            Poco::Data::Keywords::now;
    }
    EXPECT_EQ(stamp_after, kSentinel)
        << "an unchanged group was rewritten by the re-cluster";

    // The unchanged group also keeps its identity and membership.
    bool found = false;
    for (const auto &g : second.groups)
    {
        if (g.group.id != stable_group_id)
            continue;
        found = true;
        EXPECT_EQ(g.members.size(), 2u);
    }
    EXPECT_TRUE(found) << "stable group disappeared across the re-cluster";

    db_manager_.reset();
    std::filesystem::remove(db_path);
}

// The 77k-hairball regression test. A file that bridges two groups must join
// exactly ONE of them, not fuse them: under the old connected-components
// grouping, A~bridge~B made A and B one group however unlike each other they
// were, and repeated across a real library that collapsed 60% of every grouped
// file into a single "duplicate group". Representative-anchored grouping bounds
// each group to files that resemble its representative, so the bridge picks its
// best match and the two groups stay separate.
TEST_F(DuplicateFinderRangeTest, BridgeFileJoinsOneGroupInsteadOfMergingThem)
{
    std::string db_path = (std::filesystem::temp_directory_path() / "df_incremental_merge.sqlite").string();
    std::filesystem::remove(db_path);
    db_manager_ = std::make_unique<DatabaseManager>(db_path);
    ASSERT_TRUE(db_manager_->initialize());

    ASSERT_TRUE(ScannedFilesOps::ensureTable(*db_manager_));
    ASSERT_TRUE(DuplicateGroupsOps::ensureTables(*db_manager_));
    ASSERT_TRUE(ImageArtifactsOps::ensureTable(*db_manager_));
    config_->setPropertyValue("duplicates.finder.enabled", true);
    config_->setPropertyValue("duplicates.threshold.similarity", 0.85);
    config_->setPropertyValue("duplicates.threshold.duplicate", 0.95);

    auto addProcessedFile = [&](const std::string &path, const std::vector<float> &vec) {
        ScannedFileRow row;
        row.file_path = path;
        row.relative_path = path;
        row.file_name = path;
        row.location_key = "test_location";
        row.file_metadata = "{\"sizeBytes\": 1000}";
        row.processed = 2;
        EXPECT_TRUE(ScannedFilesOps::upsert(*db_manager_, row));
        auto stored = ScannedFilesOps::getByPath(*db_manager_, path);

        ImageEmbeddingRecord rec;
        rec.file_path = path;
        rec.model = "test-model";
        rec.dim = static_cast<int>(vec.size());
        rec.embedding_blob.resize(vec.size() * sizeof(float));
        std::memcpy(rec.embedding_blob.data(), vec.data(), rec.embedding_blob.size());
        EXPECT_TRUE(ImageArtifactsOps::upsertEmbedding(*db_manager_, rec));
        return stored ? stored->id : 0;
    };

    // Directions A and B sit at cos = 0.5 — well under threshold, so two
    // distinct groups. Their bisector is cos 0.866 to both, i.e. over the 0.85
    // threshold and thus a bridge between them. Direction C is orthogonal to
    // everything and must not move.
    int id_a1 = addProcessedFile("/mrg/a1.jpg", {1.0f, 0.0f,   0.05f, 0.0f,  0.0f, 0.0f, 0.0f, 0.0f});
    int id_a2 = addProcessedFile("/mrg/a2.jpg", {1.0f, 0.0f,   0.0f,  0.05f, 0.0f, 0.0f, 0.0f, 0.0f});
    int id_b1 = addProcessedFile("/mrg/b1.jpg", {0.5f, 0.866f, 0.05f, 0.0f,  0.0f, 0.0f, 0.0f, 0.0f});
    int id_b2 = addProcessedFile("/mrg/b2.jpg", {0.5f, 0.866f, 0.0f,  0.05f, 0.0f, 0.0f, 0.0f, 0.0f});
    int id_c1 = addProcessedFile("/mrg/c1.jpg", {0.0f, 0.0f,   0.0f,  0.0f,  1.0f, 0.05f, 0.0f, 0.0f});
    int id_c2 = addProcessedFile("/mrg/c2.jpg", {0.0f, 0.0f,   0.0f,  0.0f,  1.0f, 0.0f, 0.05f, 0.0f});
    ASSERT_GT(id_a1, 0);
    ASSERT_GT(id_c2, 0);

    DuplicateFinder finder(config_, *db_manager_);
    ASSERT_TRUE(finder.initialize());
    finder.findDuplicates();

    auto before = DuplicateGroupsOps::getGroupsWithMembers(*db_manager_, 0, 10, "EMBEDDING");
    ASSERT_EQ(before.groups.size(), 3u) << "expected the A, B and C pairs as three groups";

    int c_group_id = -1;
    for (const auto &g : before.groups)
        for (const auto &m : g.members)
            if (m.file_id == id_c1)
                c_group_id = g.group.id;
    ASSERT_GT(c_group_id, 0);

    // Back-date C's row; an incremental run must not rewrite it.
    const std::string kSentinel = "1999-01-01 00:00:00";
    {
        auto lease = db_manager_->acquireSessionLease();
        std::string sentinel = kSentinel;
        Poco::Data::Statement stamp(lease.get());
        stamp << "UPDATE duplicate_groups SET updated_at = ? WHERE id = ?",
            Poco::Data::Keywords::use(sentinel), Poco::Data::Keywords::use(c_group_id),
            Poco::Data::Keywords::now;
    }

    // The bridge: cos 0.866 to both A and B.
    int id_bridge = addProcessedFile("/mrg/bridge.jpg", {0.866f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f});
    ASSERT_GT(id_bridge, 0);
    finder.findDuplicates();

    // The untouched group must be recomputed and found identical, not rewritten.
    EXPECT_GT(finder.lastReclusterGroupsUnchanged(), 0)
        << "no group was recognised as unchanged; the sentinel check below proves nothing";

    auto after = DuplicateGroupsOps::getGroupsWithMembers(*db_manager_, 0, 10, "EMBEDDING");
    ASSERT_EQ(after.groups.size(), 3u)
        << "the bridge must not fuse A and B; all three groups must survive";

    int bridge_groups = 0;
    bool c_found = false;
    for (const auto &g : after.groups)
    {
        std::set<int> ids;
        for (const auto &m : g.members)
            ids.insert(m.file_id);

        if (ids.count(id_bridge))
        {
            bridge_groups++;
            // Whichever side it picked, it took only that side with it.
            const bool joined_a = (ids == std::set<int>{id_a1, id_a2, id_bridge});
            const bool joined_b = (ids == std::set<int>{id_b1, id_b2, id_bridge});
            EXPECT_TRUE(joined_a || joined_b)
                << "bridge pulled in members of both groups";
        }
        else if (ids.count(id_c1))
        {
            c_found = true;
            EXPECT_EQ(g.group.id, c_group_id) << "unrelated group changed identity";
            EXPECT_EQ(ids, (std::set<int>{id_c1, id_c2}));
        }
    }
    EXPECT_EQ(bridge_groups, 1) << "single membership: the bridge belongs to exactly one group";
    EXPECT_TRUE(c_found) << "unrelated group was deleted";

    std::string stamp_after;
    {
        auto lease = db_manager_->acquireSessionLease();
        Poco::Data::Statement q(lease.get());
        q << "SELECT updated_at FROM duplicate_groups WHERE id = ?",
            Poco::Data::Keywords::into(stamp_after), Poco::Data::Keywords::use(c_group_id),
            Poco::Data::Keywords::now;
    }
    EXPECT_EQ(stamp_after, kSentinel) << "unrelated group was rewritten";

    db_manager_.reset();
    std::filesystem::remove(db_path);
}

// Grouping must be a pure function of (files, threshold). The sweep walks
// candidates in purity order rather than arrival order precisely so that
// re-importing the same library in a different sequence cannot produce
// different groups — the property the old greedy first-match-wins grouping
// lacked, and the reason it needed split/redistribute repair passes.
TEST_F(DuplicateFinderRangeTest, GroupingIsIndependentOfInsertionOrder)
{
    // Two clusters, deliberately far apart, plus a loner that groups with
    // nobody. Distinct file sizes give the purity order something to rank on.
    struct Spec { std::string name; std::vector<float> vec; int64_t size; };
    const std::vector<Spec> specs = {
        {"a1", {1.0f, 0.0f, 0.03f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}, 5000},
        {"a2", {1.0f, 0.0f, 0.0f, 0.03f, 0.0f, 0.0f, 0.0f, 0.0f}, 9000},
        {"a3", {1.0f, 0.02f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}, 7000},
        {"b1", {0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.03f, 0.0f, 0.0f}, 4000},
        {"b2", {0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.03f, 0.0f}, 8000},
        {"lone", {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f}, 6000},
    };

    // Returns "rep_name:member_names,..." per group, sorted — a canonical
    // fingerprint of the grouping that ignores group ids and row order.
    auto groupingFingerprint = [&](const std::vector<std::size_t> &order) {
        std::string db_path = (std::filesystem::temp_directory_path() /
                               ("df_order_" + std::to_string(order[0]) + std::to_string(order[1]) + ".sqlite"))
                                  .string();
        std::filesystem::remove(db_path);
        db_manager_ = std::make_unique<DatabaseManager>(db_path);
        EXPECT_TRUE(db_manager_->initialize());
        EXPECT_TRUE(ScannedFilesOps::ensureTable(*db_manager_));
        EXPECT_TRUE(DuplicateGroupsOps::ensureTables(*db_manager_));
        EXPECT_TRUE(ImageArtifactsOps::ensureTable(*db_manager_));
        config_->setPropertyValue("duplicates.threshold.similarity", 0.85);
        config_->setPropertyValue("duplicates.threshold.duplicate", 0.95);

        std::map<int, std::string> name_of;
        for (std::size_t idx : order)
        {
            const Spec &sp = specs[idx];
            const std::string path = "/ord/" + sp.name + ".jpg";
            ScannedFileRow row;
            row.file_path = path;
            row.relative_path = path;
            row.file_name = path;
            row.location_key = "test_location";
            row.file_metadata = "{\"sizeBytes\": " + std::to_string(sp.size) + "}";
            row.processed = 2;
            EXPECT_TRUE(ScannedFilesOps::upsert(*db_manager_, row));
            auto stored = ScannedFilesOps::getByPath(*db_manager_, path);
            if (stored) name_of[stored->id] = sp.name;

            ImageEmbeddingRecord rec;
            rec.file_path = path;
            rec.model = "test-model";
            rec.dim = static_cast<int>(sp.vec.size());
            rec.embedding_blob.resize(sp.vec.size() * sizeof(float));
            std::memcpy(rec.embedding_blob.data(), sp.vec.data(), rec.embedding_blob.size());
            EXPECT_TRUE(ImageArtifactsOps::upsertEmbedding(*db_manager_, rec));
        }

        DuplicateFinder finder(config_, *db_manager_);
        EXPECT_TRUE(finder.initialize());
        finder.findDuplicates();

        auto page = DuplicateGroupsOps::getGroupsWithMembers(*db_manager_, 0, 50, "EMBEDDING");
        std::vector<std::string> lines;
        for (const auto &g : page.groups)
        {
            std::string rep;
            std::vector<std::string> names;
            for (const auto &m : g.members)
            {
                names.push_back(name_of[m.file_id]);
                if (m.is_representative) rep = name_of[m.file_id];
            }
            std::sort(names.begin(), names.end());
            std::string line = rep + ":";
            for (const auto &n : names) line += n + ",";
            lines.push_back(line);
        }
        std::sort(lines.begin(), lines.end());
        std::string fingerprint;
        for (const auto &l : lines) fingerprint += l + "|";

        db_manager_.reset();
        std::filesystem::remove(db_path);
        return fingerprint;
    };

    const std::string forward  = groupingFingerprint({0, 1, 2, 3, 4, 5});
    const std::string reversed = groupingFingerprint({5, 4, 3, 2, 1, 0});
    const std::string shuffled = groupingFingerprint({3, 0, 5, 2, 4, 1});

    EXPECT_FALSE(forward.empty()) << "no groups formed; the fixture proves nothing";
    EXPECT_EQ(forward, reversed) << "insertion order changed the grouping";
    EXPECT_EQ(forward, shuffled) << "insertion order changed the grouping";

    // And the representative is the purest member: same size ranking applies,
    // so within cluster A the largest file (a2, 9000) must lead.
    EXPECT_NE(forward.find("a2:"), std::string::npos)
        << "representative is not the purest member of its group: " << forward;
}
