// DF-2 (backend_optimization_plan.md): the sqlite-vec KNN index becomes real —
// backfilled/purged at startup (reconcile), kept honest on deletion, and used
// as the PRIMARY candidate generator. These tests exercise the index itself;
// the KNN grouping path is covered end-to-end in test_duplicate_finder_range.
// All tests skip when the vec0 extension is not present.

#include <gtest/gtest.h>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "database/database_manager.hpp"
#include "database/image_artifacts_ops.hpp"
#include "database/scanned_files_ops.hpp"
#include "database/vector_index.hpp"

using namespace MediaDedup;

namespace
{
    std::string findVec0()
    {
        for (const char *p : {"extensions/vec0.dylib", "../extensions/vec0.dylib",
                              "../../dedup_server/extensions/vec0.dylib"})
        {
            if (std::filesystem::exists(p))
                return std::filesystem::absolute(p).string();
        }
        return "";
    }

    std::vector<std::uint8_t> vecBytes(std::vector<float> v)
    {
        std::vector<std::uint8_t> b(v.size() * sizeof(float));
        std::memcpy(b.data(), v.data(), b.size());
        return b;
    }

    std::string tempDb(const char *name)
    {
        auto p = std::filesystem::temp_directory_path() / name;
        std::filesystem::remove(p);
        return p.string();
    }
} // namespace

TEST(VectorIndexTest, InsertQueryRemoveRoundtrip)
{
    std::string ext = findVec0();
    if (ext.empty())
        GTEST_SKIP() << "vec0 extension not found";

    std::string db_path = tempDb("vec_roundtrip.sqlite");
    VectorIndex vix;
    ASSERT_TRUE(vix.init(db_path, ext, 4));
    ASSERT_TRUE(vix.isReady());

    auto a = vecBytes({1.0f, 0.0f, 0.0f, 0.0f});
    auto b = vecBytes({0.9f, 0.1f, 0.0f, 0.0f});
    auto c = vecBytes({0.0f, 0.0f, 1.0f, 0.0f});
    ASSERT_TRUE(vix.insert(101, a.data(), a.size()));
    ASSERT_TRUE(vix.insert(102, b.data(), b.size()));
    ASSERT_TRUE(vix.insert(103, c.data(), c.size()));

    auto knn = vix.queryKnn(a.data(), a.size(), 3);
    ASSERT_EQ(knn.size(), 3u);
    EXPECT_EQ(knn[0].first, 101) << "self must be the nearest neighbor";
    EXPECT_NEAR(knn[0].second, 0.0, 1e-5);
    EXPECT_EQ(knn[1].first, 102) << "near-parallel vector ranks second";
    EXPECT_GT(knn[2].second, 0.5) << "orthogonal vector is distant under cosine";

    ASSERT_TRUE(vix.remove(102));
    knn = vix.queryKnn(a.data(), a.size(), 3);
    ASSERT_EQ(knn.size(), 2u);
    for (const auto &p : knn)
        EXPECT_NE(p.first, 102) << "removed vector must not surface";

    ASSERT_TRUE(vix.removeMany({101, 103}));
    EXPECT_TRUE(vix.queryKnn(a.data(), a.size(), 3).empty());

    vix.close();
    std::filesystem::remove(db_path);
}

TEST(VectorIndexTest, ReconcileBackfillsAndPurges)
{
    std::string ext = findVec0();
    if (ext.empty())
        GTEST_SKIP() << "vec0 extension not found";

    std::string db_path = tempDb("vec_reconcile.sqlite");

    // Build a main DB with two embedded files — WITHOUT the vector index
    // enabled, mirroring a library embedded before the index existed.
    {
        DatabaseManager dbm(db_path);
        ASSERT_TRUE(dbm.initialize());
        ASSERT_TRUE(ScannedFilesOps::ensureTable(dbm));
        ASSERT_TRUE(ImageArtifactsOps::ensureTable(dbm));

        for (int i = 0; i < 2; ++i)
        {
            ScannedFileRow row;
            row.file_path = "/vec/f" + std::to_string(i) + ".jpg";
            row.file_name = "f" + std::to_string(i) + ".jpg";
            row.location_key = "mediaLocation:test";
            row.processed = 2;
            ASSERT_TRUE(ScannedFilesOps::upsert(dbm, row));

            ImageEmbeddingRecord rec;
            rec.file_path = row.file_path;
            rec.model = "m";
            rec.dim = 4;
            rec.embedding_blob = vecBytes({1.0f * (i + 1), 0.5f, 0.0f, 0.0f});
            ASSERT_TRUE(ImageArtifactsOps::upsertEmbedding(dbm, rec));
        }
    }

    VectorIndex vix;
    ASSERT_TRUE(vix.init(db_path, ext, 4));

    // A stale vector for a file that does not exist (the pre-DF-2 world:
    // deletions never removed index rows).
    auto stale = vecBytes({0.0f, 0.0f, 0.0f, 1.0f});
    ASSERT_TRUE(vix.insert(999999, stale.data(), stale.size()));

    VectorIndex::ReconcileStats stats;
    ASSERT_TRUE(vix.reconcile(stats));
    EXPECT_EQ(stats.inserted, 2) << "both embedded files backfilled";
    EXPECT_EQ(stats.purged, 1) << "stale row purged";

    // Idempotent: a second pass changes nothing.
    ASSERT_TRUE(vix.reconcile(stats));
    EXPECT_EQ(stats.inserted, 0);
    EXPECT_EQ(stats.purged, 0);

    // The backfilled vectors are searchable and the stale one is gone.
    auto probe = vecBytes({1.0f, 0.5f, 0.0f, 0.0f});
    auto knn = vix.queryKnn(probe.data(), probe.size(), 4);
    ASSERT_EQ(knn.size(), 2u);
    for (const auto &p : knn)
        EXPECT_NE(p.first, 999999);

    vix.close();
    std::filesystem::remove(db_path);
}
