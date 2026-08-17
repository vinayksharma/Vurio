#include <gtest/gtest.h>
#include "orchestration/duplicate_finder.hpp"
#include "database/sql_constants.hpp"
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
 * DF-3 migration: a pre-edge-list install (files already discovered, groups,
 * but an EMPTY duplicate_edges table) must have discovery re-opened at
 * initialize so it repopulates the edges — while the existing groups stay
 * visible. DISC-1: "discovery has run" is read off the per-file flag, since
 * there is no watermark left to read it from.
 */
TEST_F(DuplicateFinderRangeTest, EmptyEdgeTableWithDiscoveredFilesReopensDiscovery)
{
    ASSERT_TRUE(DuplicateGroupsOps::ensureTables(*db_manager_));

    int file_id = mockScannedFile("/test/legacy_rep.jpg");
    int group_id = DuplicateGroupsOps::createGroup(
        *db_manager_, "EMBEDDING", file_id, "/test/legacy_rep.jpg", 1000, "2025-01-01", 0.96);
    ASSERT_GT(group_id, 0);
    {
        auto lease = db_manager_->acquireSessionLease();
        Poco::Data::Statement s(lease.get());
        s << "UPDATE scanned_files SET discovery_done = 1 WHERE id = " + std::to_string(file_id),
            Poco::Data::Keywords::now;
    }

    DuplicateFinder finder(config_, *db_manager_);
    ASSERT_TRUE(finder.initialize());

    int still_discovered = 0;
    {
        auto lease = db_manager_->acquireSessionLease();
        Poco::Data::Statement q(lease.get());
        q << "SELECT COUNT(*) FROM scanned_files WHERE discovery_done = 1",
            Poco::Data::Keywords::into(still_discovered), Poco::Data::Keywords::now;
    }
    EXPECT_EQ(0, still_discovered)
        << "discovered files with no edges must be re-opened for one-time re-discovery";
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

    // DISC-1: "this batch is done" is per-file state, not a checkpoint id.
    EXPECT_EQ(ScannedFilesOps::countDiscovered(*db_manager_), 3)
        << "every processed file in the batch must be marked discovered";

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

namespace
{
    // The fixtures below all want the same thing: a fully processed file
    // with an embedding, written the way the real pipeline writes it.
    int addProcessedFileWithEmbedding(DatabaseManager &db, const std::string &path,
                                      const std::vector<float> &vec, int64_t size_bytes = 1000)
    {
        ScannedFileRow row;
        row.file_path = path;
        row.relative_path = path;
        row.file_name = path;
        row.location_key = "test_location";
        row.file_metadata = "{\"sizeBytes\": " + std::to_string(size_bytes) + "}";
        row.processed = 2;
        EXPECT_TRUE(ScannedFilesOps::upsert(db, row));
        auto stored = ScannedFilesOps::getByPath(db, path);

        if (!vec.empty())
        {
            ImageEmbeddingRecord rec;
            rec.file_path = path;
            rec.model = "test-model";
            rec.dim = static_cast<int>(vec.size());
            rec.embedding_blob.resize(vec.size() * sizeof(float));
            std::memcpy(rec.embedding_blob.data(), vec.data(), rec.embedding_blob.size());
            EXPECT_TRUE(ImageArtifactsOps::upsertEmbedding(db, rec));
        }
        return stored ? stored->id : 0;
    }

    // Some fixtures need the real pipeline ORDER, which the helper above collapses:
    // the row appears first (unprocessed, no vector), the embedding is written
    // later, and processed only flips to 2 after that.
    int addUnprocessedFile(DatabaseManager &db, const std::string &path, int64_t size_bytes = 1000)
    {
        ScannedFileRow row;
        row.file_path = path;
        row.relative_path = path;
        row.file_name = path;
        row.location_key = "test_location";
        row.file_metadata = "{\"sizeBytes\": " + std::to_string(size_bytes) + "}";
        row.processed = 0;
        EXPECT_TRUE(ScannedFilesOps::upsert(db, row));
        auto stored = ScannedFilesOps::getByPath(db, path);
        return stored ? stored->id : 0;
    }

    void writeEmbedding(DatabaseManager &db, const std::string &path, const std::vector<float> &vec)
    {
        ImageEmbeddingRecord rec;
        rec.file_path = path;
        rec.model = "test-model";
        rec.dim = static_cast<int>(vec.size());
        rec.embedding_blob.resize(vec.size() * sizeof(float));
        std::memcpy(rec.embedding_blob.data(), vec.data(), rec.embedding_blob.size());
        EXPECT_TRUE(ImageArtifactsOps::upsertEmbedding(db, rec));
    }

    int scalarQuery(DatabaseManager &db, const std::string &sql)
    {
        auto lease = db.acquireSessionLease();
        int value = 0;
        Poco::Data::Statement q(lease.get());
        q << sql, Poco::Data::Keywords::into(value), Poco::Data::Keywords::now;
        return value;
    }

    void execSql(DatabaseManager &db, const std::string &sql)
    {
        auto lease = db.acquireSessionLease();
        Poco::Data::Statement s(lease.get());
        s << sql, Poco::Data::Keywords::now;
    }
} // namespace

// Join guard: the reported symptom in miniature. One afternoon's shooting — two
// frames, each kept as a RAW/JPG pair matching at ~0.999, with the frames
// themselves ~0.93 apart. Every file clears the 0.92 threshold against the
// first representative, so plain rep-anchored membership swallows all four
// into one group and files each image under something it resembles far less
// than its own twin. The join guard is what keeps the pairs apart. (This
// shape accounted for 41% of the groups in the sample library, so it is the
// common case, not a curiosity.)
TEST_F(DuplicateFinderRangeTest, JoinGuardSplitsBurstFramesIntoTheirRealPairs)
{
    std::string db_path = (std::filesystem::temp_directory_path() / "df4_join_margin.sqlite").string();
    std::filesystem::remove(db_path);
    db_manager_ = std::make_unique<DatabaseManager>(db_path);
    ASSERT_TRUE(db_manager_->initialize());
    ASSERT_TRUE(ScannedFilesOps::ensureTable(*db_manager_));
    ASSERT_TRUE(DuplicateGroupsOps::ensureTables(*db_manager_));
    ASSERT_TRUE(ImageArtifactsOps::ensureTable(*db_manager_));

    config_->setPropertyValue("duplicates.finder.enabled", true);
    config_->setPropertyValue("duplicates.threshold.similarity", 0.92);
    config_->setPropertyValue("duplicates.threshold.duplicate", 0.97);
    if (!config_->hasProperty("duplicates.group.joinDeficitRatio"))
        config_->createProperty("duplicates.group.joinDeficitRatio", 2.0, "test");
    config_->setPropertyValue("duplicates.group.joinDeficitRatio", 2.0);

    // Frame A along e0, frame B at cos 0.93 to it; each frame twice, with tiny
    // orthogonal perturbations standing in for RAW-vs-JPG rendering
    // differences (intra-pair ~0.9996, cross-frame ~0.9298).
    const int id_a_raw = addProcessedFileWithEmbedding(
        *db_manager_, "/df4/A.raf", {1.0f, 0.0f, 0.02f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f});
    const int id_a_jpg = addProcessedFileWithEmbedding(
        *db_manager_, "/df4/A.jpg", {1.0f, 0.0f, 0.0f, 0.02f, 0.0f, 0.0f, 0.0f, 0.0f});
    const int id_b_raw = addProcessedFileWithEmbedding(
        *db_manager_, "/df4/B.raf", {0.93f, 0.36756f, 0.0f, 0.0f, 0.02f, 0.0f, 0.0f, 0.0f});
    const int id_b_jpg = addProcessedFileWithEmbedding(
        *db_manager_, "/df4/B.jpg", {0.93f, 0.36756f, 0.0f, 0.0f, 0.0f, 0.02f, 0.0f, 0.0f});
    ASSERT_GT(id_a_raw, 0);
    ASSERT_GT(id_b_jpg, 0);

    DuplicateFinder finder(config_, *db_manager_);
    ASSERT_TRUE(finder.initialize());
    finder.findDuplicates();

    auto page = DuplicateGroupsOps::getGroupsWithMembers(*db_manager_, 0, 10, "EMBEDDING");
    ASSERT_EQ(page.groups.size(), 2u) << "the two frames must not share a group";
    for (const auto &g : page.groups)
    {
        std::set<int> ids;
        for (const auto &m : g.members)
            ids.insert(m.file_id);
        const bool is_a = (ids == std::set<int>{id_a_raw, id_a_jpg});
        const bool is_b = (ids == std::set<int>{id_b_raw, id_b_jpg});
        EXPECT_TRUE(is_a || is_b) << "a group mixes the two frames";
        for (const auto &m : g.members)
            EXPECT_GT(m.similarity_score, 0.99)
                << "every member must be a near-twin of its representative, not a scene match";
    }

    // All four edges are on disk either way — the split is a clustering
    // decision, so turning the guard off must re-fuse them from the SAME
    // recorded edges, with no re-comparison.
    EXPECT_EQ(scalarQuery(*db_manager_, "SELECT COUNT(*) FROM duplicate_edges"), 6)
        << "all six pairs cleared the discovery floor";

    config_->setPropertyValue("duplicates.group.joinDeficitRatio", 0.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    finder.findDuplicates();

    auto fused = DuplicateGroupsOps::getGroupsWithMembers(*db_manager_, 0, 10, "EMBEDDING");
    ASSERT_EQ(fused.groups.size(), 1u) << "a disabled guard must restore plain rep-anchored membership";
    EXPECT_EQ(fused.groups[0].members.size(), 4u);

    db_manager_.reset();
    std::filesystem::remove(db_path);
}

// Join guard: the case a fixed score margin cannot express. The twins sit at 0.9991
// and the third file at 0.9951 — a gap of 0.004, which any usable absolute
// margin waves through, yet the representative is five times further away
// than the twin. Measuring the ratio of distances catches it; measuring the
// difference of scores cannot, because 0.004 is also the width of pure noise
// down at the 0.93 end of the same scale.
TEST_F(DuplicateFinderRangeTest, JoinGuardSeparatesNearTwinsFromAFiveTimesWorseRepresentative)
{
    std::string db_path = (std::filesystem::temp_directory_path() / "df4_tight_band.sqlite").string();
    std::filesystem::remove(db_path);
    db_manager_ = std::make_unique<DatabaseManager>(db_path);
    ASSERT_TRUE(db_manager_->initialize());
    ASSERT_TRUE(ScannedFilesOps::ensureTable(*db_manager_));
    ASSERT_TRUE(DuplicateGroupsOps::ensureTables(*db_manager_));
    ASSERT_TRUE(ImageArtifactsOps::ensureTable(*db_manager_));
    config_->setPropertyValue("duplicates.finder.enabled", true);
    config_->setPropertyValue("duplicates.threshold.similarity", 0.92);
    config_->setPropertyValue("duplicates.threshold.duplicate", 0.97);
    if (!config_->hasProperty("duplicates.group.joinDeficitRatio"))
        config_->createProperty("duplicates.group.joinDeficitRatio", 2.0, "test");
    config_->setPropertyValue("duplicates.group.joinDeficitRatio", 2.0);

    // Biggest file, so purity order makes it the first representative.
    const int id_rep = addProcessedFileWithEmbedding(
        *db_manager_, "/df4tb/wide.jpg", {1.0f, 0.0f, 0.0f, 0.0f, 0.095f, 0.0f, 0.0f, 0.0f}, 9000);
    const int id_t1 = addProcessedFileWithEmbedding(
        *db_manager_, "/df4tb/twin1.jpg", {1.0f, 0.0f, 0.03f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}, 7000);
    const int id_t2 = addProcessedFileWithEmbedding(
        *db_manager_, "/df4tb/twin2.jpg", {1.0f, 0.0f, 0.0f, 0.03f, 0.0f, 0.0f, 0.0f, 0.0f}, 5000);
    ASSERT_GT(id_rep, 0);
    ASSERT_GT(id_t2, 0);

    DuplicateFinder finder(config_, *db_manager_);
    ASSERT_TRUE(finder.initialize());
    finder.findDuplicates();

    auto page = DuplicateGroupsOps::getGroupsWithMembers(*db_manager_, 0, 10, "EMBEDDING");
    ASSERT_EQ(page.groups.size(), 1u);
    std::set<int> ids;
    for (const auto &m : page.groups[0].members)
        ids.insert(m.file_id);
    EXPECT_EQ(ids, (std::set<int>{id_t1, id_t2}))
        << "the twins must group with each other, not with a representative five times further away";
    EXPECT_EQ(ids.count(id_rep), 0u);

    db_manager_.reset();
    std::filesystem::remove(db_path);
}
// ---------------------------------------------------------------------------
// DISC-1: per-file edge-discovery state (docs/todo_discovery_watermark.md).
//
// Discovery used to decide "which files still need comparing" from a single
// ascending id watermark, which is only correct if processed=2 arrives in id
// order — it does not. The fixtures below are the scenarios that hole produced,
// re-run against scanned_files.discovery_done. None of them may rely on a
// trigger or a requeue table: those are gone, and their absence is asserted.
// ---------------------------------------------------------------------------

namespace
{
    // The stopgap's own artifacts. Nothing in the new design may depend on
    // them, and the migration must remove them from existing databases.
    bool legacyRequeueArtifactsExist(DatabaseManager &db)
    {
        return scalarQuery(db,
                           "SELECT COUNT(*) FROM sqlite_master WHERE name IN "
                           "('duplicate_discovery_requeue', "
                           " 'trg_scanned_files_requeue_late_processed')") > 0;
    }

    int discoveryDone(DatabaseManager &db, int file_id)
    {
        return scalarQuery(db, "SELECT discovery_done FROM scanned_files WHERE id = " +
                                   std::to_string(file_id));
    }

    // Recreate the exact database state a pre-DISC-1 install would be in, so
    // the migration has something real to convert.
    void createLegacyRequeueTable(DatabaseManager &db)
    {
        execSql(db, "CREATE TABLE IF NOT EXISTS duplicate_discovery_requeue ("
                    "file_id INTEGER PRIMARY KEY, reason TEXT NOT NULL DEFAULT '', "
                    "requeued_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP) WITHOUT ROWID");
    }
} // namespace

// A vector that lands after its peers were discovered is invisible to them,
// and under the old watermark nobody ever looked again. reconcile() backfills
// the index and clears the flag; re-querying from that one side must restore
// the pair, because edges are symmetric.
TEST_F(DuplicateFinderRangeTest, ClearingTheFlagRestoresEdgesTheWatermarkCouldNotReach)
{
    std::string db_path = (std::filesystem::temp_directory_path() / "disc1_reopen.sqlite").string();
    std::filesystem::remove(db_path);
    db_manager_ = std::make_unique<DatabaseManager>(db_path);
    ASSERT_TRUE(db_manager_->initialize());
    ASSERT_TRUE(ScannedFilesOps::ensureTable(*db_manager_));
    ASSERT_TRUE(DuplicateGroupsOps::ensureTables(*db_manager_));
    ASSERT_TRUE(ImageArtifactsOps::ensureTable(*db_manager_));
    config_->setPropertyValue("duplicates.finder.enabled", true);
    config_->setPropertyValue("duplicates.threshold.similarity", 0.92);
    config_->setPropertyValue("duplicates.threshold.duplicate", 0.97);

    const int id_a = addProcessedFileWithEmbedding(
        *db_manager_, "/disc1re/a.jpg", {1.0f, 0.0f, 0.02f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f});
    const int id_a2 = addProcessedFileWithEmbedding(
        *db_manager_, "/disc1re/a2.jpg", {1.0f, 0.0f, 0.0f, 0.02f, 0.0f, 0.0f, 0.0f, 0.0f});
    ASSERT_GT(id_a, 0);
    ASSERT_GT(id_a2, 0);

    DuplicateFinder finder(config_, *db_manager_);
    ASSERT_TRUE(finder.initialize());
    finder.findDuplicates();
    ASSERT_EQ(scalarQuery(*db_manager_, "SELECT COUNT(*) FROM duplicate_edges"), 1);
    ASSERT_EQ(DuplicateGroupsOps::countGroupsByMode(*db_manager_, "EMBEDDING"), 1);
    ASSERT_EQ(discoveryDone(*db_manager_, id_a), 1);
    ASSERT_EQ(discoveryDone(*db_manager_, id_a2), 1);

    // Reproduce the damage: the pair's edge was never recorded, because one of
    // them was not visible to the other at discovery time.
    execSql(*db_manager_, "DELETE FROM duplicate_edges");
    DuplicateGroupsOps::deleteGroupsByMode(*db_manager_, "EMBEDDING");

    // Both files are flagged discovered, so an ordinary run has nothing to do.
    finder.findDuplicates();
    EXPECT_EQ(scalarQuery(*db_manager_, "SELECT COUNT(*) FROM duplicate_edges"), 0)
        << "a file whose pass already committed must not be re-compared for free";
    EXPECT_EQ(DuplicateGroupsOps::countGroupsByMode(*db_manager_, "EMBEDDING"), 0);

    // Reopen ONE side — what VectorIndex::reconcile() does for a backfilled
    // vector. Edges are symmetric, so that alone must restore the pair.
    execSql(*db_manager_, "UPDATE scanned_files SET discovery_done = 0 WHERE id = " +
                              std::to_string(id_a2));
    finder.findDuplicates();

    EXPECT_EQ(scalarQuery(*db_manager_, "SELECT COUNT(*) FROM duplicate_edges"), 1)
        << "the reopened file must rediscover the neighbour that never looked back";
    EXPECT_EQ(DuplicateGroupsOps::countGroupsByMode(*db_manager_, "EMBEDDING"), 1)
        << "the repaired edge must re-form the group";
    EXPECT_EQ(discoveryDone(*db_manager_, id_a2), 1)
        << "a completed pass must set the flag, or the file is re-compared forever";

    db_manager_.reset();
    std::filesystem::remove(db_path);
}

// A file marked processed before its embedding was written used to be stepped
// over silently — the watermark advanced past it and nothing looked again. It
// must now simply stay unflagged, with no queue row to park and no backlog to
// re-join on every run, until its vector exists.
TEST_F(DuplicateFinderRangeTest, ProcessedFileWithoutEmbeddingIsHeldBackNotDropped)
{
    std::string db_path = (std::filesystem::temp_directory_path() / "disc1_no_embedding.sqlite").string();
    std::filesystem::remove(db_path);
    db_manager_ = std::make_unique<DatabaseManager>(db_path);
    ASSERT_TRUE(db_manager_->initialize());
    ASSERT_TRUE(ScannedFilesOps::ensureTable(*db_manager_));
    ASSERT_TRUE(DuplicateGroupsOps::ensureTables(*db_manager_));
    ASSERT_TRUE(ImageArtifactsOps::ensureTable(*db_manager_));
    config_->setPropertyValue("duplicates.finder.enabled", true);
    config_->setPropertyValue("duplicates.threshold.similarity", 0.92);
    config_->setPropertyValue("duplicates.threshold.duplicate", 0.97);

    const int id_a = addProcessedFileWithEmbedding(
        *db_manager_, "/disc1ne/a.jpg", {1.0f, 0.0f, 0.02f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f});
    // Processed, but the embedding has not landed yet.
    const int id_late = addProcessedFileWithEmbedding(*db_manager_, "/disc1ne/late.jpg", {});
    ASSERT_GT(id_a, 0);
    ASSERT_GT(id_late, 0);

    DuplicateFinder finder(config_, *db_manager_);
    ASSERT_TRUE(finder.initialize());
    finder.findDuplicates();

    EXPECT_EQ(discoveryDone(*db_manager_, id_a), 1)
        << "one embedding-less file must not stall discovery for the library";
    EXPECT_EQ(discoveryDone(*db_manager_, id_late), 0)
        << "a file with nothing to compare is still owed a pass";

    // The embedding finally lands. Nothing else is needed: the selector picks
    // the file up as soon as it has a vector.
    writeEmbedding(*db_manager_, "/disc1ne/late.jpg",
                   {1.0f, 0.0f, 0.0f, 0.02f, 0.0f, 0.0f, 0.0f, 0.0f});
    finder.findDuplicates();

    auto page = DuplicateGroupsOps::getGroupsWithMembers(*db_manager_, 0, 10, "EMBEDDING");
    ASSERT_EQ(page.groups.size(), 1u) << "the late file must group once its embedding exists";
    std::set<int> ids;
    for (const auto &m : page.groups[0].members)
        ids.insert(m.file_id);
    EXPECT_EQ(ids, (std::set<int>{id_a, id_late}));
    EXPECT_EQ(discoveryDone(*db_manager_, id_late), 1);

    db_manager_.reset();
    std::filesystem::remove(db_path);
}

// Existing libraries carry edges recorded by an earlier, incomplete discovery,
// so an epoch bump re-opens every file for one repair pass. It must keep the
// edges and groups it already has — the pass is additive, not a wipe.
TEST_F(DuplicateFinderRangeTest, EdgeDiscoveryEpochBumpReopensEveryFileWithoutWiping)
{
    std::string db_path = (std::filesystem::temp_directory_path() / "disc1_epoch.sqlite").string();
    std::filesystem::remove(db_path);
    db_manager_ = std::make_unique<DatabaseManager>(db_path);
    ASSERT_TRUE(db_manager_->initialize());
    ASSERT_TRUE(ScannedFilesOps::ensureTable(*db_manager_));
    ASSERT_TRUE(DuplicateGroupsOps::ensureTables(*db_manager_));
    ASSERT_TRUE(ImageArtifactsOps::ensureTable(*db_manager_));
    config_->setPropertyValue("duplicates.finder.enabled", true);
    config_->setPropertyValue("duplicates.threshold.similarity", 0.92);
    config_->setPropertyValue("duplicates.threshold.duplicate", 0.97);

    ASSERT_GT(addProcessedFileWithEmbedding(*db_manager_, "/disc1ep/a.jpg",
                                            {1.0f, 0.0f, 0.02f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}), 0);
    ASSERT_GT(addProcessedFileWithEmbedding(*db_manager_, "/disc1ep/a2.jpg",
                                            {1.0f, 0.0f, 0.0f, 0.02f, 0.0f, 0.0f, 0.0f, 0.0f}), 0);
    ASSERT_GT(addProcessedFileWithEmbedding(*db_manager_, "/disc1ep/b.jpg",
                                            {0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}), 0);

    {
        DuplicateFinder finder(config_, *db_manager_);
        ASSERT_TRUE(finder.initialize());
        finder.findDuplicates();
    }
    const int edges_before = scalarQuery(*db_manager_, "SELECT COUNT(*) FROM duplicate_edges");
    ASSERT_GT(edges_before, 0);
    ASSERT_EQ(DuplicateGroupsOps::countGroupsByMode(*db_manager_, "EMBEDDING"), 1);
    ASSERT_EQ(scalarQuery(*db_manager_, "SELECT COUNT(*) FROM scanned_files WHERE discovery_done = 0"), 0)
        << "a database created at the current epoch owes no repair";

    // Pretend this database was written by the pre-fix build.
    execSql(*db_manager_, "UPDATE duplicate_edges_meta SET discovery_epoch = 0 WHERE id = 1");

    DuplicateFinder upgraded(config_, *db_manager_);
    ASSERT_TRUE(upgraded.initialize());

    EXPECT_EQ(scalarQuery(*db_manager_, "SELECT COUNT(*) FROM scanned_files WHERE discovery_done = 0"), 3)
        << "every file must be re-opened for the one-time repair pass";
    EXPECT_EQ(scalarQuery(*db_manager_, "SELECT COUNT(*) FROM duplicate_edges"), edges_before)
        << "the repair must not wipe the edges it already has";
    EXPECT_EQ(DuplicateGroupsOps::countGroupsByMode(*db_manager_, "EMBEDDING"), 1)
        << "the repair must not wipe existing groups";

    // repairProgress() is only published WHILE a run is in flight, so the
    // observable contract from here is that the indicator is not stuck on
    // either side of one.
    EXPECT_EQ(upgraded.repairProgress().total, 0)
        << "no episode is in flight until a run starts";

    upgraded.findDuplicates();
    EXPECT_EQ(scalarQuery(*db_manager_, "SELECT COUNT(*) FROM scanned_files WHERE discovery_done = 0"), 0)
        << "the repair pass must drain in one run";
    EXPECT_EQ(scalarQuery(*db_manager_, "SELECT COUNT(*) FROM duplicate_edges"), edges_before);
    EXPECT_EQ(DuplicateGroupsOps::countGroupsByMode(*db_manager_, "EMBEDDING"), 1);
    EXPECT_EQ(upgraded.repairProgress().total, 0)
        << "a finished episode must clear the indicator";

    db_manager_.reset();
    std::filesystem::remove(db_path);
}

// The reported group-27423 failure, reduced. processed is written per file_path
// by a parallel pool, so a file can reach processed=2 long after a higher id
// was discovered. Under the id watermark it was then never selected again: it
// never queried its own neighbours, so its true twin — sitting behind the same
// hole — was never linked to it, and the join guard read a best-available score
// that was simply wrong. With per-file state the next run just picks it up.
TEST_F(DuplicateFinderRangeTest, FileProcessedAfterDiscoveryPassedItIsStillDiscovered)
{
    std::string db_path = (std::filesystem::temp_directory_path() / "disc1_late_processed.sqlite").string();
    std::filesystem::remove(db_path);
    db_manager_ = std::make_unique<DatabaseManager>(db_path);
    ASSERT_TRUE(db_manager_->initialize());
    ASSERT_TRUE(ScannedFilesOps::ensureTable(*db_manager_));
    ASSERT_TRUE(DuplicateGroupsOps::ensureTables(*db_manager_));
    ASSERT_TRUE(ImageArtifactsOps::ensureTable(*db_manager_));
    config_->setPropertyValue("duplicates.finder.enabled", true);
    config_->setPropertyValue("duplicates.threshold.similarity", 0.92);
    config_->setPropertyValue("duplicates.threshold.duplicate", 0.97);

    // twin_b is scanned between the other two but is still being processed
    // when discovery runs: no vector yet, so no peer can see it either.
    const int id_a = addProcessedFileWithEmbedding(
        *db_manager_, "/disc1lp/twin_a.jpg", {1.0f, 0.0f, 0.02f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f});
    const int id_twin = addUnprocessedFile(*db_manager_, "/disc1lp/twin_b.jpg");
    const int id_other = addProcessedFileWithEmbedding(
        *db_manager_, "/disc1lp/other.jpg", {0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f});
    ASSERT_GT(id_a, 0);
    ASSERT_GT(id_twin, id_a);
    ASSERT_GT(id_other, id_twin);

    DuplicateFinder finder(config_, *db_manager_);
    ASSERT_TRUE(finder.initialize());
    finder.findDuplicates();

    ASSERT_EQ(discoveryDone(*db_manager_, id_other), 1)
        << "a file waiting on processing must not hold up the ones behind it";
    ASSERT_EQ(discoveryDone(*db_manager_, id_twin), 0)
        << "a file that is not processed yet is still owed a pass";
    ASSERT_EQ(DuplicateGroupsOps::countGroupsByMode(*db_manager_, "EMBEDDING"), 0);

    // Processing finishes: vector first, then the status flip — the order the
    // real pipeline uses. Its id is now far behind the last file discovered.
    writeEmbedding(*db_manager_, "/disc1lp/twin_b.jpg",
                   {1.0f, 0.0f, 0.0f, 0.02f, 0.0f, 0.0f, 0.0f, 0.0f});
    execSql(*db_manager_, "UPDATE scanned_files SET processed = 2 WHERE id = " + std::to_string(id_twin));

    EXPECT_FALSE(legacyRequeueArtifactsExist(*db_manager_))
        << "the repair must not depend on a trigger or a queue table";

    finder.findDuplicates();

    EXPECT_EQ(scalarQuery(*db_manager_,
                          "SELECT COUNT(*) FROM duplicate_edges WHERE file_a = " + std::to_string(id_a) +
                              " AND file_b = " + std::to_string(id_twin)),
              1)
        << "the pair the watermark could never reach must be linked";
    EXPECT_EQ(DuplicateGroupsOps::countGroupsByMode(*db_manager_, "EMBEDDING"), 1)
        << "the restored edge must form the group";
    EXPECT_EQ(discoveryDone(*db_manager_, id_twin), 1);

    db_manager_.reset();
    std::filesystem::remove(db_path);
}

// THE RACE THE LATE-PROCESSED TRIGGER COULD NOT SEE, and the reason DISC-1 is a fix and
// not a cleanup. That trigger parked files reaching processed=2 below the
// PERSISTED watermark — but the checkpoint is only written after a batch
// commits, so a file finishing DURING a batch was still ahead of the stored
// value (no requeue) while the in-memory watermark sailed past its id. Once it
// sits at processed=2 the trigger can never fire for it again, so the file was
// lost permanently. This fixture reproduces exactly that end state: processed,
// embedded, id behind the watermark, nothing queued.
TEST_F(DuplicateFinderRangeTest, FileFinishingDuringADiscoveryBatchIsNotLost)
{
    std::string db_path = (std::filesystem::temp_directory_path() / "disc1_batch_race.sqlite").string();
    std::filesystem::remove(db_path);
    db_manager_ = std::make_unique<DatabaseManager>(db_path);
    ASSERT_TRUE(db_manager_->initialize());
    ASSERT_TRUE(ScannedFilesOps::ensureTable(*db_manager_));
    ASSERT_TRUE(DuplicateGroupsOps::ensureTables(*db_manager_));
    ASSERT_TRUE(ImageArtifactsOps::ensureTable(*db_manager_));
    config_->setPropertyValue("duplicates.finder.enabled", true);
    config_->setPropertyValue("duplicates.threshold.similarity", 0.92);
    config_->setPropertyValue("duplicates.threshold.duplicate", 0.97);

    const int id_a = addProcessedFileWithEmbedding(
        *db_manager_, "/disc1br/twin_a.jpg", {1.0f, 0.0f, 0.02f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f});
    const int id_twin = addUnprocessedFile(*db_manager_, "/disc1br/twin_b.jpg");
    const int id_other = addProcessedFileWithEmbedding(
        *db_manager_, "/disc1br/other.jpg", {0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f});
    ASSERT_GT(id_twin, id_a);
    ASSERT_GT(id_other, id_twin);

    DuplicateFinder finder(config_, *db_manager_);
    ASSERT_TRUE(finder.initialize());

    // The batch is in flight: it snapshotted {twin_a, other} and is comparing
    // them, so the checkpoint on disk is still 0 — the trigger's `id <=
    // watermark` test would be FALSE for twin_b and park nothing.
    execSql(*db_manager_, "UPDATE duplicate_processing_checkpoint SET last_processed_id = 0");
    writeEmbedding(*db_manager_, "/disc1br/twin_b.jpg",
                   {1.0f, 0.0f, 0.0f, 0.02f, 0.0f, 0.0f, 0.0f, 0.0f});
    execSql(*db_manager_, "UPDATE scanned_files SET processed = 2 WHERE id = " + std::to_string(id_twin));

    // The batch commits and the watermark jumps past twin_b's id. From here the
    // old selector (`id > watermark`) can never reach it, and the trigger
    // cannot fire again because processed is already 2.
    execSql(*db_manager_, "UPDATE duplicate_processing_checkpoint SET last_processed_id = " +
                              std::to_string(id_other));

    finder.findDuplicates();

    EXPECT_EQ(discoveryDone(*db_manager_, id_twin), 1)
        << "a file that finished mid-batch must still get its own pass";
    EXPECT_EQ(scalarQuery(*db_manager_,
                          "SELECT COUNT(*) FROM duplicate_edges WHERE file_a = " + std::to_string(id_a) +
                              " AND file_b = " + std::to_string(id_twin)),
              1)
        << "the twins must be linked; this edge is the one the race lost forever";
    EXPECT_EQ(DuplicateGroupsOps::countGroupsByMode(*db_manager_, "EMBEDDING"), 1);

    db_manager_.reset();
    std::filesystem::remove(db_path);
}

// The normal path must not need any special handling: a newly processed file is
// discovered by the ordinary selector, once, with nothing parked anywhere.
TEST_F(DuplicateFinderRangeTest, NewlyProcessedFileIsDiscoveredWithoutAnyQueue)
{
    std::string db_path = (std::filesystem::temp_directory_path() / "disc1_ahead.sqlite").string();
    std::filesystem::remove(db_path);
    db_manager_ = std::make_unique<DatabaseManager>(db_path);
    ASSERT_TRUE(db_manager_->initialize());
    ASSERT_TRUE(ScannedFilesOps::ensureTable(*db_manager_));
    ASSERT_TRUE(DuplicateGroupsOps::ensureTables(*db_manager_));
    ASSERT_TRUE(ImageArtifactsOps::ensureTable(*db_manager_));
    config_->setPropertyValue("duplicates.finder.enabled", true);
    config_->setPropertyValue("duplicates.threshold.similarity", 0.92);
    config_->setPropertyValue("duplicates.threshold.duplicate", 0.97);

    const int id_a = addProcessedFileWithEmbedding(
        *db_manager_, "/disc1ah/a.jpg", {1.0f, 0.0f, 0.02f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f});
    ASSERT_GT(id_a, 0);

    DuplicateFinder finder(config_, *db_manager_);
    ASSERT_TRUE(finder.initialize());
    finder.findDuplicates();
    ASSERT_EQ(discoveryDone(*db_manager_, id_a), 1);
    ASSERT_FALSE(legacyRequeueArtifactsExist(*db_manager_));

    const int id_new = addUnprocessedFile(*db_manager_, "/disc1ah/b.jpg");
    ASSERT_GT(id_new, id_a);
    writeEmbedding(*db_manager_, "/disc1ah/b.jpg", {1.0f, 0.0f, 0.0f, 0.02f, 0.0f, 0.0f, 0.0f, 0.0f});
    execSql(*db_manager_, "UPDATE scanned_files SET processed = 2 WHERE id = " + std::to_string(id_new));

    EXPECT_EQ(discoveryDone(*db_manager_, id_a), 1)
        << "an unrelated file finishing must not re-open work that is already done";

    finder.findDuplicates();
    EXPECT_EQ(DuplicateGroupsOps::countGroupsByMode(*db_manager_, "EMBEDDING"), 1)
        << "the ordinary path must still group it";
    EXPECT_EQ(discoveryDone(*db_manager_, id_new), 1);
    EXPECT_EQ(scalarQuery(*db_manager_, "SELECT COUNT(*) FROM duplicate_edges"), 1)
        << "the pair must be recorded exactly once, from one side";

    db_manager_.reset();
    std::filesystem::remove(db_path);
}

// A bulk error reset (processed < 0 -> 0) sends files back through processing.
// When they come out the far side at 2 again, their id is behind everything
// already discovered — the exact shape the watermark could not express. No
// trigger is involved: reprocessing rewrites the embedding, which reopens them.
TEST_F(DuplicateFinderRangeTest, ErrorResetLeavesTheFileDiscoverable)
{
    std::string db_path = (std::filesystem::temp_directory_path() / "disc1_error_reset.sqlite").string();
    std::filesystem::remove(db_path);
    db_manager_ = std::make_unique<DatabaseManager>(db_path);
    ASSERT_TRUE(db_manager_->initialize());
    ASSERT_TRUE(ScannedFilesOps::ensureTable(*db_manager_));
    ASSERT_TRUE(DuplicateGroupsOps::ensureTables(*db_manager_));
    ASSERT_TRUE(ImageArtifactsOps::ensureTable(*db_manager_));
    config_->setPropertyValue("duplicates.finder.enabled", true);
    config_->setPropertyValue("duplicates.threshold.similarity", 0.92);
    config_->setPropertyValue("duplicates.threshold.duplicate", 0.97);

    const int id_a = addProcessedFileWithEmbedding(
        *db_manager_, "/disc1er/a.jpg", {1.0f, 0.0f, 0.02f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f});
    // Errored, so it has no vector and cannot be compared to anything yet.
    const int id_err = addUnprocessedFile(*db_manager_, "/disc1er/errored.jpg");
    execSql(*db_manager_, "UPDATE scanned_files SET processed = -6 WHERE id = " + std::to_string(id_err));
    const int id_c = addProcessedFileWithEmbedding(
        *db_manager_, "/disc1er/c.jpg", {0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f});
    ASSERT_GT(id_c, id_err);

    DuplicateFinder finder(config_, *db_manager_);
    ASSERT_TRUE(finder.initialize());
    finder.findDuplicates();
    ASSERT_EQ(DuplicateGroupsOps::countGroupsByMode(*db_manager_, "EMBEDDING"), 0);

    // "Reset all errors", then a successful reprocess.
    EXPECT_GE(ScannedFilesOps::resetAllErrors(*db_manager_), 1);
    writeEmbedding(*db_manager_, "/disc1er/errored.jpg",
                   {1.0f, 0.0f, 0.0f, 0.02f, 0.0f, 0.0f, 0.0f, 0.0f});
    execSql(*db_manager_, "UPDATE scanned_files SET processed = 2 WHERE id = " + std::to_string(id_err));

    finder.findDuplicates();

    EXPECT_EQ(discoveryDone(*db_manager_, id_err), 1);
    EXPECT_EQ(scalarQuery(*db_manager_,
                          "SELECT COUNT(*) FROM duplicate_edges WHERE file_a = " + std::to_string(id_a) +
                              " AND file_b = " + std::to_string(id_err)),
              1)
        << "a file that came back from an error reset must find its neighbours";
    EXPECT_EQ(DuplicateGroupsOps::countGroupsByMode(*db_manager_, "EMBEDDING"), 1);

    db_manager_.reset();
    std::filesystem::remove(db_path);
}

// Restoring an ejected drive rewrites processed back to 0 for every path on the
// volume, so those files reprocess and reach 2 again well behind the rest of
// the library. Same requirement as the error reset, different writer — and the
// point of per-file state is that neither needs its own hook.
TEST_F(DuplicateFinderRangeTest, DriveRestoreLeavesTheFileDiscoverable)
{
    std::string db_path = (std::filesystem::temp_directory_path() / "disc1_restore.sqlite").string();
    std::filesystem::remove(db_path);
    db_manager_ = std::make_unique<DatabaseManager>(db_path);
    ASSERT_TRUE(db_manager_->initialize());
    ASSERT_TRUE(ScannedFilesOps::ensureTable(*db_manager_));
    ASSERT_TRUE(DuplicateGroupsOps::ensureTables(*db_manager_));
    ASSERT_TRUE(ImageArtifactsOps::ensureTable(*db_manager_));
    config_->setPropertyValue("duplicates.finder.enabled", true);
    config_->setPropertyValue("duplicates.threshold.similarity", 0.92);
    config_->setPropertyValue("duplicates.threshold.duplicate", 0.97);

    const int id_restored = addProcessedFileWithEmbedding(
        *db_manager_, "/disc1dr/restored.jpg", {1.0f, 0.0f, 0.02f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f});
    const int id_later = addProcessedFileWithEmbedding(
        *db_manager_, "/disc1dr/later.jpg", {1.0f, 0.0f, 0.0f, 0.02f, 0.0f, 0.0f, 0.0f, 0.0f});
    ASSERT_GT(id_later, id_restored);

    DuplicateFinder finder(config_, *db_manager_);
    ASSERT_TRUE(finder.initialize());
    finder.findDuplicates();
    ASSERT_EQ(DuplicateGroupsOps::countGroupsByMode(*db_manager_, "EMBEDDING"), 1);

    // The volume is ejected: edges and the group go with the rows.
    execSql(*db_manager_, "DELETE FROM duplicate_edges");
    DuplicateGroupsOps::deleteGroupsByMode(*db_manager_, "EMBEDDING");

    // Undo-eject: the path comes back as unprocessed (eject_service does
    // exactly this), then reprocesses.
    execSql(*db_manager_, "UPDATE scanned_files SET processed = 0 WHERE id = " +
                              std::to_string(id_restored));
    writeEmbedding(*db_manager_, "/disc1dr/restored.jpg",
                   {1.0f, 0.0f, 0.02f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f});
    execSql(*db_manager_, "UPDATE scanned_files SET processed = 2 WHERE id = " +
                              std::to_string(id_restored));

    finder.findDuplicates();

    EXPECT_EQ(scalarQuery(*db_manager_, "SELECT COUNT(*) FROM duplicate_edges"), 1)
        << "the restored file must be compared again and re-link its twin";
    EXPECT_EQ(DuplicateGroupsOps::countGroupsByMode(*db_manager_, "EMBEDDING"), 1);

    db_manager_.reset();
    std::filesystem::remove(db_path);
}

// DF-3 item 5, finally closed: a re-embedded file used to keep the edges its
// OLD vector produced, because the watermark had long passed it. Writing a new
// embedding must reopen it.
TEST_F(DuplicateFinderRangeTest, ReEmbeddingReopensAFileForDiscovery)
{
    std::string db_path = (std::filesystem::temp_directory_path() / "disc1_reembed.sqlite").string();
    std::filesystem::remove(db_path);
    db_manager_ = std::make_unique<DatabaseManager>(db_path);
    ASSERT_TRUE(db_manager_->initialize());
    ASSERT_TRUE(ScannedFilesOps::ensureTable(*db_manager_));
    ASSERT_TRUE(DuplicateGroupsOps::ensureTables(*db_manager_));
    ASSERT_TRUE(ImageArtifactsOps::ensureTable(*db_manager_));
    config_->setPropertyValue("duplicates.finder.enabled", true);
    config_->setPropertyValue("duplicates.threshold.similarity", 0.92);
    config_->setPropertyValue("duplicates.threshold.duplicate", 0.97);

    const int id_a = addProcessedFileWithEmbedding(
        *db_manager_, "/disc1rb/a.jpg", {1.0f, 0.0f, 0.02f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f});
    // Unrelated to a.jpg at first: nothing to group.
    const int id_b = addProcessedFileWithEmbedding(
        *db_manager_, "/disc1rb/b.jpg", {0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f});
    ASSERT_GT(id_b, id_a);

    DuplicateFinder finder(config_, *db_manager_);
    ASSERT_TRUE(finder.initialize());
    finder.findDuplicates();
    ASSERT_EQ(scalarQuery(*db_manager_, "SELECT COUNT(*) FROM duplicate_edges"), 0);
    ASSERT_EQ(discoveryDone(*db_manager_, id_b), 1);

    // A corrected model re-embeds b.jpg, and it is now a twin of a.jpg.
    writeEmbedding(*db_manager_, "/disc1rb/b.jpg", {1.0f, 0.0f, 0.0f, 0.02f, 0.0f, 0.0f, 0.0f, 0.0f});
    EXPECT_EQ(discoveryDone(*db_manager_, id_b), 0)
        << "a rewritten vector invalidates the pass that was run against the old one";

    finder.findDuplicates();
    EXPECT_EQ(scalarQuery(*db_manager_,
                          "SELECT COUNT(*) FROM duplicate_edges WHERE file_a = " + std::to_string(id_a) +
                              " AND file_b = " + std::to_string(id_b)),
              1)
        << "the re-embedded file must be compared against its neighbours again";
    EXPECT_EQ(DuplicateGroupsOps::countGroupsByMode(*db_manager_, "EMBEDDING"), 1);

    db_manager_.reset();
    std::filesystem::remove(db_path);
}

// "Reset duplicates" and the floor-provenance reset both go through
// resetCheckpoint(). Zeroing the checkpoint alone would now reset nothing, and
// the next run would find no work at all.
TEST_F(DuplicateFinderRangeTest, ResetCheckpointReopensEveryFileForDiscovery)
{
    std::string db_path = (std::filesystem::temp_directory_path() / "disc1_reset.sqlite").string();
    std::filesystem::remove(db_path);
    db_manager_ = std::make_unique<DatabaseManager>(db_path);
    ASSERT_TRUE(db_manager_->initialize());
    ASSERT_TRUE(ScannedFilesOps::ensureTable(*db_manager_));
    ASSERT_TRUE(DuplicateGroupsOps::ensureTables(*db_manager_));
    ASSERT_TRUE(ImageArtifactsOps::ensureTable(*db_manager_));
    config_->setPropertyValue("duplicates.finder.enabled", true);
    config_->setPropertyValue("duplicates.threshold.similarity", 0.92);
    config_->setPropertyValue("duplicates.threshold.duplicate", 0.97);

    ASSERT_GT(addProcessedFileWithEmbedding(*db_manager_, "/disc1rs/a.jpg",
                                            {1.0f, 0.0f, 0.02f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}), 0);
    ASSERT_GT(addProcessedFileWithEmbedding(*db_manager_, "/disc1rs/a2.jpg",
                                            {1.0f, 0.0f, 0.0f, 0.02f, 0.0f, 0.0f, 0.0f, 0.0f}), 0);

    DuplicateFinder finder(config_, *db_manager_);
    ASSERT_TRUE(finder.initialize());
    finder.findDuplicates();
    ASSERT_EQ(scalarQuery(*db_manager_, "SELECT COUNT(*) FROM scanned_files WHERE discovery_done = 1"), 2);

    ASSERT_TRUE(DuplicateGroupsOps::resetCheckpoint(*db_manager_, "EMBEDDING"));
    EXPECT_EQ(scalarQuery(*db_manager_, "SELECT COUNT(*) FROM scanned_files WHERE discovery_done = 1"), 0)
        << "resetting the checkpoint must reopen discovery, or the next run does nothing";

    execSql(*db_manager_, "DELETE FROM duplicate_edges");
    finder.findDuplicates();
    EXPECT_EQ(scalarQuery(*db_manager_, "SELECT COUNT(*) FROM duplicate_edges"), 1)
        << "the run after a reset must rebuild the edge list from scratch";

    db_manager_.reset();
    std::filesystem::remove(db_path);
}

// Upgrading a pre-DISC-1 database: everything the old watermark legitimately
// covered is already discovered, anything still parked in the requeue was owed
// a pass and must stay pending, and the stopgap's table and trigger must go.
TEST_F(DuplicateFinderRangeTest, MigrationOffTheIdWatermarkPreservesWhatWasOwed)
{
    std::string db_path = (std::filesystem::temp_directory_path() / "disc1_migrate.sqlite").string();
    std::filesystem::remove(db_path);
    db_manager_ = std::make_unique<DatabaseManager>(db_path);
    ASSERT_TRUE(db_manager_->initialize());
    ASSERT_TRUE(ScannedFilesOps::ensureTable(*db_manager_));
    ASSERT_TRUE(DuplicateGroupsOps::ensureTables(*db_manager_));
    ASSERT_TRUE(ImageArtifactsOps::ensureTable(*db_manager_));

    const int id_done = addProcessedFileWithEmbedding(
        *db_manager_, "/disc1mg/done.jpg", {1.0f, 0.0f, 0.02f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f});
    const int id_owed = addProcessedFileWithEmbedding(
        *db_manager_, "/disc1mg/owed.jpg", {1.0f, 0.0f, 0.0f, 0.02f, 0.0f, 0.0f, 0.0f, 0.0f});
    const int id_ahead = addProcessedFileWithEmbedding(
        *db_manager_, "/disc1mg/ahead.jpg", {0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f});
    ASSERT_GT(id_owed, id_done);
    ASSERT_GT(id_ahead, id_owed);

    // Rebuild the pre-DISC-1 state by hand: a watermark that covered the first
    // two files, one of them parked in the requeue, and no flags anywhere.
    execSql(*db_manager_, "UPDATE scanned_files SET discovery_done = 0");
    createLegacyRequeueTable(*db_manager_);
    execSql(*db_manager_, "INSERT OR IGNORE INTO duplicate_discovery_requeue(file_id, reason) VALUES(" +
                              std::to_string(id_owed) + ", 'processed_below_watermark')");
    execSql(*db_manager_,
            "INSERT INTO duplicate_processing_checkpoint(mode, last_processed_id) VALUES('EMBEDDING', " +
                std::to_string(id_owed) + ") ON CONFLICT(mode) DO UPDATE SET last_processed_id = " +
                std::to_string(id_owed));

    ASSERT_TRUE(DuplicateGroupsOps::ensureDiscoveryState(*db_manager_));

    EXPECT_EQ(discoveryDone(*db_manager_, id_done), 1)
        << "a file the watermark legitimately covered must not be re-compared";
    EXPECT_EQ(discoveryDone(*db_manager_, id_owed), 0)
        << "a file still sitting in the requeue was owed a pass and must stay pending";
    EXPECT_EQ(discoveryDone(*db_manager_, id_ahead), 0)
        << "a file past the watermark had never been discovered";
    EXPECT_FALSE(legacyRequeueArtifactsExist(*db_manager_))
        << "the stopgap's table and trigger must be gone once their state is folded in";

    // Idempotent: running it again must not undo the migration or crash.
    ASSERT_TRUE(DuplicateGroupsOps::ensureDiscoveryState(*db_manager_));
    EXPECT_EQ(discoveryDone(*db_manager_, id_done), 1);
    EXPECT_EQ(discoveryDone(*db_manager_, id_owed), 0);

    db_manager_.reset();
    std::filesystem::remove(db_path);
}

// The epoch is stamped at initialize(), but the sweep it triggers can run for
// hours. An in-memory "repair in flight" flag was therefore lost on any restart
// in between, and nothing re-armed it because the stamp was already current —
// the first real epoch-3 sweep ran its full 3 h 45 m with the indicator dark.
// The marker has to outlive the process.
TEST_F(DuplicateFinderRangeTest, RepairEpisodeSurvivesARestartMidSweep)
{
    std::string db_path = (std::filesystem::temp_directory_path() / "disc1_repair_restart.sqlite").string();
    std::filesystem::remove(db_path);
    db_manager_ = std::make_unique<DatabaseManager>(db_path);
    ASSERT_TRUE(db_manager_->initialize());
    ASSERT_TRUE(ScannedFilesOps::ensureTable(*db_manager_));
    ASSERT_TRUE(DuplicateGroupsOps::ensureTables(*db_manager_));
    ASSERT_TRUE(ImageArtifactsOps::ensureTable(*db_manager_));
    config_->setPropertyValue("duplicates.finder.enabled", true);
    config_->setPropertyValue("duplicates.threshold.similarity", 0.92);
    config_->setPropertyValue("duplicates.threshold.duplicate", 0.97);

    ASSERT_GT(addProcessedFileWithEmbedding(*db_manager_, "/disc1rr/a.jpg",
                                            {1.0f, 0.0f, 0.02f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}), 0);
    ASSERT_GT(addProcessedFileWithEmbedding(*db_manager_, "/disc1rr/a2.jpg",
                                            {1.0f, 0.0f, 0.0f, 0.02f, 0.0f, 0.0f, 0.0f, 0.0f}), 0);

    auto pending = [&] {
        return scalarQuery(*db_manager_,
                           "SELECT COALESCE(discovery_repair_pending, 0) FROM duplicate_edges_meta WHERE id = 1");
    };

    {
        DuplicateFinder finder(config_, *db_manager_);
        ASSERT_TRUE(finder.initialize());
        finder.findDuplicates();
    }
    ASSERT_EQ(pending(), 0) << "an ordinary run is not a repair episode";

    // Pretend this database was written by a pre-fix build, then start up.
    execSql(*db_manager_, "UPDATE duplicate_edges_meta SET discovery_epoch = 0 WHERE id = 1");
    {
        DuplicateFinder seeding(config_, *db_manager_);
        ASSERT_TRUE(seeding.initialize());
        // ...and die before running a single batch, which is what happened for
        // real: the epoch was stamped by one process and swept by another.
    }
    EXPECT_EQ(pending(), 1) << "the epoch bump must record the owed sweep durably";
    EXPECT_EQ(scalarQuery(*db_manager_, "SELECT COUNT(*) FROM scanned_files WHERE discovery_done = 0"), 2);

    // Restart. The stamp is already current, so the epoch branch cannot fire
    // again — only the persisted marker can tell this process a sweep is owed.
    DuplicateFinder restarted(config_, *db_manager_);
    ASSERT_TRUE(restarted.initialize());
    EXPECT_EQ(pending(), 1)
        << "a restart between the stamp and the sweep must not lose the episode";

    restarted.findDuplicates();
    EXPECT_EQ(pending(), 0) << "a drained sweep must clear the marker";
    EXPECT_EQ(scalarQuery(*db_manager_, "SELECT COUNT(*) FROM scanned_files WHERE discovery_done = 0"), 0);
    EXPECT_EQ(restarted.repairProgress().total, 0) << "a finished episode must clear the indicator";

    db_manager_.reset();
    std::filesystem::remove(db_path);
}

// A fresh database has no watermark to migrate from, so nothing may be marked
// discovered — every file must go through a real pass.
TEST_F(DuplicateFinderRangeTest, FreshDatabaseStartsWithNothingDiscovered)
{
    std::string db_path = (std::filesystem::temp_directory_path() / "disc1_fresh.sqlite").string();
    std::filesystem::remove(db_path);
    db_manager_ = std::make_unique<DatabaseManager>(db_path);
    ASSERT_TRUE(db_manager_->initialize());
    ASSERT_TRUE(ScannedFilesOps::ensureTable(*db_manager_));
    ASSERT_TRUE(DuplicateGroupsOps::ensureTables(*db_manager_));
    ASSERT_TRUE(ImageArtifactsOps::ensureTable(*db_manager_));

    EXPECT_FALSE(legacyRequeueArtifactsExist(*db_manager_))
        << "a fresh database must never create the stopgap's table or trigger";

    const int id_a = addProcessedFileWithEmbedding(
        *db_manager_, "/disc1fr/a.jpg", {1.0f, 0.0f, 0.02f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f});
    EXPECT_EQ(discoveryDone(*db_manager_, id_a), 0);

    db_manager_.reset();
    std::filesystem::remove(db_path);
}

// ---------------------------------------------------------------------------
// DF-4: orphan edges are removed by the code that deletes the file, not by a
// full-table DELETE scan at the top of every finder run.
//
// The per-run purge (DELETE ... WHERE file_a NOT IN (SELECT id ...) OR ...)
// scanned the whole edge table under SQLite's WRITE lock every 5 s — ~30 s at
// 14.9M edges — and starved every other writer in the process (thumbnail
// cache rows, EXIF updates, processed-state marks all hit busy_timeout).
// ---------------------------------------------------------------------------

// Deleting a scanned file removes its edges (in either column — edges are
// canonical file_a < file_b) in the same transaction and flags a re-cluster,
// which the next run consumes and acts on.
TEST_F(DuplicateFinderRangeTest, DeletingAFileRemovesItsEdgesAndSchedulesARecluster)
{
    std::string db_path = (std::filesystem::temp_directory_path() / "df4_delete.sqlite").string();
    std::filesystem::remove(db_path);
    db_manager_ = std::make_unique<DatabaseManager>(db_path);
    ASSERT_TRUE(db_manager_->initialize());
    ASSERT_TRUE(ScannedFilesOps::ensureTable(*db_manager_));
    ASSERT_TRUE(DuplicateGroupsOps::ensureTables(*db_manager_));
    ASSERT_TRUE(ImageArtifactsOps::ensureTable(*db_manager_));
    config_->setPropertyValue("duplicates.finder.enabled", true);
    config_->setPropertyValue("duplicates.threshold.similarity", 0.92);
    config_->setPropertyValue("duplicates.threshold.duplicate", 0.97);

    const int id_a = addProcessedFileWithEmbedding(
        *db_manager_, "/df4/a.jpg", {1.0f, 0.0f, 0.02f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f});
    const int id_b = addProcessedFileWithEmbedding(
        *db_manager_, "/df4/b.jpg", {1.0f, 0.0f, 0.0f, 0.02f, 0.0f, 0.0f, 0.0f, 0.0f});
    const int id_c = addProcessedFileWithEmbedding(
        *db_manager_, "/df4/c.jpg", {1.0f, 0.0f, 0.0f, 0.0f, 0.02f, 0.0f, 0.0f, 0.0f});
    ASSERT_GT(id_a, 0);
    ASSERT_GT(id_b, 0);
    ASSERT_GT(id_c, 0);

    DuplicateFinder finder(config_, *db_manager_);
    ASSERT_TRUE(finder.initialize());
    finder.findDuplicates();
    ASSERT_EQ(scalarQuery(*db_manager_, "SELECT COUNT(*) FROM duplicate_edges"), 3) << "a-b, a-c, b-c";
    ASSERT_EQ(DuplicateGroupsOps::countGroupsByMode(*db_manager_, "EMBEDDING"), 1);
    ASSERT_EQ(scalarQuery(*db_manager_, "SELECT COALESCE(regroup_pending,0) FROM duplicate_edges_meta WHERE id = 1"), 0);

    // Delete the middle-id file: it is file_b of (a,b) and file_a of (b,c),
    // so both index paths are exercised.
    ASSERT_TRUE(ScannedFilesOps::removeByPath(*db_manager_, "/df4/b.jpg"));
    EXPECT_EQ(scalarQuery(*db_manager_, "SELECT COUNT(*) FROM duplicate_edges WHERE file_a = " + std::to_string(id_b) +
                                            " OR file_b = " + std::to_string(id_b)), 0)
        << "the deleted file's edges go with it, in the same transaction";
    EXPECT_EQ(scalarQuery(*db_manager_, "SELECT COUNT(*) FROM duplicate_edges"), 1) << "a-c survives";
    EXPECT_EQ(scalarQuery(*db_manager_, "SELECT regroup_pending FROM duplicate_edges_meta WHERE id = 1"), 1)
        << "removing edges must request a re-cluster (a removed edge can split a component)";

    // The next run consumes the request and re-clusters from the surviving
    // edges: a-c is still a group, and the flag is cleared.
    finder.findDuplicates();
    EXPECT_EQ(scalarQuery(*db_manager_, "SELECT regroup_pending FROM duplicate_edges_meta WHERE id = 1"), 0);
    EXPECT_EQ(DuplicateGroupsOps::countGroupsByMode(*db_manager_, "EMBEDDING"), 1);
    EXPECT_EQ(scalarQuery(*db_manager_, "SELECT COUNT(*) FROM duplicate_members"), 2) << "a and c";

    // Deleting a file whose edges were the group's only links dissolves it.
    ASSERT_TRUE(ScannedFilesOps::removeByPath(*db_manager_, "/df4/c.jpg"));
    EXPECT_EQ(scalarQuery(*db_manager_, "SELECT COUNT(*) FROM duplicate_edges"), 0);
    EXPECT_EQ(scalarQuery(*db_manager_, "SELECT regroup_pending FROM duplicate_edges_meta WHERE id = 1"), 1);
    finder.findDuplicates();
    EXPECT_EQ(DuplicateGroupsOps::countGroupsByMode(*db_manager_, "EMBEDDING"), 0)
        << "the re-cluster must drop the group whose edges are gone";

    // A run with nothing pending must not touch the flag or the edge table.
    finder.findDuplicates();
    EXPECT_EQ(scalarQuery(*db_manager_, "SELECT regroup_pending FROM duplicate_edges_meta WHERE id = 1"), 0);

    db_manager_.reset();
    std::filesystem::remove(db_path);
}

// Removing a whole media location takes its edges too (both columns).
TEST_F(DuplicateFinderRangeTest, RemovingALocationRemovesItsEdges)
{
    std::string db_path = (std::filesystem::temp_directory_path() / "df4_location.sqlite").string();
    std::filesystem::remove(db_path);
    db_manager_ = std::make_unique<DatabaseManager>(db_path);
    ASSERT_TRUE(db_manager_->initialize());
    ASSERT_TRUE(ScannedFilesOps::ensureTable(*db_manager_));
    ASSERT_TRUE(DuplicateGroupsOps::ensureTables(*db_manager_));
    ASSERT_TRUE(ImageArtifactsOps::ensureTable(*db_manager_));
    // removeByLocationKey sweeps every per-file table; create the ones this
    // fixture does not otherwise need.
    execSql(*db_manager_, std::string(SQL::kCreateThumbnailCacheTable));
    execSql(*db_manager_, std::string(SQL::kCreateProcessingErrorsTable));
    execSql(*db_manager_, std::string(SQL::kCreateEjectedFilesHistoryTable));
    execSql(*db_manager_, std::string(SQL::kCreatePreviewCacheTable));

    // Two files in location L1 (ids 1,2) and one in L2 (id 3), edges 1-2, 2-3, 1-3.
    const int id_1 = addProcessedFileWithEmbedding(*db_manager_, "/L1/a.jpg", {1.0f, 0, 0, 0, 0, 0, 0, 0});
    const int id_2 = addProcessedFileWithEmbedding(*db_manager_, "/L1/b.jpg", {1.0f, 0, 0, 0, 0, 0, 0, 0});
    const int id_3 = addProcessedFileWithEmbedding(*db_manager_, "/L2/c.jpg", {1.0f, 0, 0, 0, 0, 0, 0, 0});
    execSql(*db_manager_, "UPDATE scanned_files SET location_key = 'L1' WHERE id IN (" + std::to_string(id_1) + "," + std::to_string(id_2) + ")");
    execSql(*db_manager_, "UPDATE scanned_files SET location_key = 'L2' WHERE id = " + std::to_string(id_3));
    for (auto [a, b] : std::vector<std::pair<int, int>>{{id_1, id_2}, {id_2, id_3}, {id_1, id_3}})
        execSql(*db_manager_, "INSERT INTO duplicate_edges(file_a, file_b, score) VALUES(" +
                                  std::to_string(std::min(a, b)) + "," + std::to_string(std::max(a, b)) + ", 0.99)");
    ASSERT_EQ(scalarQuery(*db_manager_, "SELECT COUNT(*) FROM duplicate_edges"), 3);

    ASSERT_TRUE(ScannedFilesOps::removeByLocationKey(*db_manager_, "L1"));
    EXPECT_EQ(scalarQuery(*db_manager_, "SELECT COUNT(*) FROM scanned_files"), 1);
    EXPECT_EQ(scalarQuery(*db_manager_, "SELECT COUNT(*) FROM duplicate_edges"), 0)
        << "every edge touching a removed file must go, whichever column it is in";

    db_manager_.reset();
    std::filesystem::remove(db_path);
}

// The finder no longer scans the edge table on every run; legacy orphans (from
// before DF-4, or written behind the server's back) are reconciled once per
// process — on the first run, off the startup path — and the DELETE only
// happens if the read-only probe finds any.
TEST_F(DuplicateFinderRangeTest, OrphanEdgesAreReconciledOncePerProcessNotEveryRun)
{
    std::string db_path = (std::filesystem::temp_directory_path() / "df4_orphans.sqlite").string();
    std::filesystem::remove(db_path);
    db_manager_ = std::make_unique<DatabaseManager>(db_path);
    ASSERT_TRUE(db_manager_->initialize());
    ASSERT_TRUE(ScannedFilesOps::ensureTable(*db_manager_));
    ASSERT_TRUE(DuplicateGroupsOps::ensureTables(*db_manager_));
    ASSERT_TRUE(ImageArtifactsOps::ensureTable(*db_manager_));
    config_->setPropertyValue("duplicates.finder.enabled", true);

    const int id_a = addProcessedFileWithEmbedding(*db_manager_, "/df4o/a.jpg", {1.0f, 0, 0, 0, 0, 0, 0, 0});
    const int id_b = addProcessedFileWithEmbedding(*db_manager_, "/df4o/b.jpg", {1.0f, 0, 0, 0, 0, 0, 0, 0});
    execSql(*db_manager_, "INSERT INTO duplicate_edges(file_a, file_b, score) VALUES(" +
                              std::to_string(std::min(id_a, id_b)) + "," + std::to_string(std::max(id_a, id_b)) + ", 0.99)");
    // A legacy orphan: endpoints that do not exist in scanned_files.
    execSql(*db_manager_, "INSERT INTO duplicate_edges(file_a, file_b, score) VALUES(999998, 999999, 0.99)");
    ASSERT_EQ(scalarQuery(*db_manager_, "SELECT COUNT(*) FROM duplicate_edges"), 2);

    {
        DuplicateFinder finder(config_, *db_manager_);
        ASSERT_TRUE(finder.initialize());
        EXPECT_EQ(scalarQuery(*db_manager_, "SELECT COUNT(*) FROM duplicate_edges"), 2)
            << "initialize() must not pay for the scan (it is on the startup path)";
        finder.findDuplicates();
        EXPECT_EQ(scalarQuery(*db_manager_, "SELECT COUNT(*) FROM duplicate_edges"), 1)
            << "the first run reconciles legacy orphans once";
        EXPECT_EQ(scalarQuery(*db_manager_, "SELECT COUNT(*) FROM duplicate_edges WHERE file_a = 999998"), 0);

        // An orphan appearing AFTER that is NOT purged by a later run: that
        // per-run scan is exactly what DF-4 removes. (It cannot arise from
        // the server's own deletion paths any more.)
        execSql(*db_manager_, "INSERT INTO duplicate_edges(file_a, file_b, score) VALUES(999996, 999997, 0.99)");
        finder.findDuplicates();
        finder.findDuplicates();
        EXPECT_EQ(scalarQuery(*db_manager_, "SELECT COUNT(*) FROM duplicate_edges WHERE file_a = 999996"), 1)
            << "findDuplicates() must not run the full-table orphan purge on every run";
    }

    // The next process (new finder instance) reconciles it on its first run.
    {
        DuplicateFinder restarted(config_, *db_manager_);
        ASSERT_TRUE(restarted.initialize());
        restarted.findDuplicates();
        EXPECT_EQ(scalarQuery(*db_manager_, "SELECT COUNT(*) FROM duplicate_edges WHERE file_a = 999996"), 0);
        EXPECT_EQ(scalarQuery(*db_manager_, "SELECT COUNT(*) FROM duplicate_edges"), 1);
    }

    // The file_b index exists (the targeted deletes depend on it).
    EXPECT_EQ(scalarQuery(*db_manager_, "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name='idx_duplicate_edges_file_b'"), 1);

    db_manager_.reset();
    std::filesystem::remove(db_path);
}
