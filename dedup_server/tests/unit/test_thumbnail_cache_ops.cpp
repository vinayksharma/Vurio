// THUMB-1 (docs/todo_aspect_ratio_thumbnails.md §4.2(b), §10): the render
// revision is part of a cached thumbnail's identity. Three guarantees:
//   (a) rows with the same (source_path, thumbnail_size) and different
//       render_revision coexist, and every revision-keyed lookup returns only
//       revision 0 today;
//   (b) upsertThumbnail succeeds and updates in place when called twice with
//       the same key — the ON CONFLICT target must name the UNIQUE constraint
//       exactly, and that only fails at RUNTIME if the two drift apart;
//   (c) ensureTable against a database holding the OLD two-column-unique
//       table drops and recreates it (the migration path).

#include <gtest/gtest.h>
#include <Poco/Data/Session.h>
#include <Poco/Data/Statement.h>
#include <ctime>
#include <filesystem>
#include <string>

#include "database/database_manager.hpp"
#include "database/sql_constants.hpp"
#include "database/thumbnail_cache_ops.hpp"

using namespace MediaDedup;
using Poco::Data::Keywords::into;
using Poco::Data::Keywords::now;

class ThumbnailCacheOpsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        db_path_ = (std::filesystem::temp_directory_path() /
                    ("mds_thumbnail_cache_ops_" + std::to_string(std::time(nullptr)) + ".sqlite"))
                       .string();
        std::filesystem::remove(db_path_);
        db_ = std::make_unique<DatabaseManager>(db_path_);
        ASSERT_TRUE(db_->initialize());
    }
    void TearDown() override
    {
        db_.reset();
        std::filesystem::remove(db_path_);
    }

    ThumbnailCacheRecord record(const std::string &src, int size, int revision, const std::string &cached)
    {
        ThumbnailCacheRecord r;
        r.source_path = src;
        r.thumbnail_size = size;
        r.render_revision = revision;
        r.cached_path = cached;
        r.file_size_bytes = 100;
        r.source_modified_at = 1;
        r.created_at = 2;
        r.last_accessed_at = 3;
        return r;
    }

    int rowCount()
    {
        auto lease = db_->acquireSessionLease();
        int n = 0;
        lease.get() << "SELECT COUNT(*) FROM thumbnail_cache", into(n), now;
        return n;
    }

    int hasRenderRevisionColumn()
    {
        auto lease = db_->acquireSessionLease();
        int n = 0;
        lease.get() << std::string(SQL::kThumbnailCacheHasRenderRevision), into(n), now;
        return n;
    }

    std::string db_path_;
    std::unique_ptr<DatabaseManager> db_;
};

TEST_F(ThumbnailCacheOpsTest, RevisionsCoexistAndLookupsReturnRevisionZero)
{
    ASSERT_TRUE(ThumbnailCacheOps::ensureTable(*db_));

    ASSERT_TRUE(ThumbnailCacheOps::upsertThumbnail(*db_, record("/media/a.jpg", 256, 0, "/cache/a_256_r0.jpg")));
    ASSERT_TRUE(ThumbnailCacheOps::upsertThumbnail(*db_, record("/media/a.jpg", 256, 1, "/cache/a_256_r1.jpg")));
    EXPECT_EQ(rowCount(), 2) << "same (path,size), different revision -> two rows";

    auto exact = ThumbnailCacheOps::getThumbnail(*db_, "/media/a.jpg", 256);
    ASSERT_TRUE(exact.has_value());
    EXPECT_EQ(exact->cached_path, "/cache/a_256_r0.jpg");
    EXPECT_EQ(exact->render_revision, 0);

    auto best = ThumbnailCacheOps::getBestFitThumbnail(*db_, "/media/a.jpg", 128);
    ASSERT_TRUE(best.has_value());
    EXPECT_EQ(best->cached_path, "/cache/a_256_r0.jpg");
    EXPECT_EQ(best->render_revision, 0);

    // The VURIO_BEST_THUMB_SUBQUERY used by the list queries filters too.
    {
        auto lease = db_->acquireSessionLease();
        std::string path;
        lease.get() << "SELECT " VURIO_BEST_THUMB_SUBQUERY("'/media/a.jpg'"), into(path), now;
        EXPECT_EQ(path, "/cache/a_256_r0.jpg");
    }

    // A file that only has a revision-1 row is a miss at revision 0.
    ASSERT_TRUE(ThumbnailCacheOps::upsertThumbnail(*db_, record("/media/b.jpg", 256, 1, "/cache/b_256_r1.jpg")));
    EXPECT_FALSE(ThumbnailCacheOps::getThumbnail(*db_, "/media/b.jpg", 256).has_value());
    EXPECT_FALSE(ThumbnailCacheOps::getBestFitThumbnail(*db_, "/media/b.jpg", 128).has_value());

    // deleteThumbnail(path, size) is revision-keyed; deleteThumbnail(path) is not.
    ASSERT_TRUE(ThumbnailCacheOps::deleteThumbnail(*db_, "/media/a.jpg", 256));
    EXPECT_EQ(rowCount(), 2) << "only the revision-0 row of a.jpg went";
    ASSERT_TRUE(ThumbnailCacheOps::deleteThumbnail(*db_, "/media/a.jpg"));
    EXPECT_EQ(rowCount(), 1) << "all revisions of a.jpg went";
}

TEST_F(ThumbnailCacheOpsTest, UpsertTwiceUpdatesInPlace)
{
    ASSERT_TRUE(ThumbnailCacheOps::ensureTable(*db_));

    ASSERT_TRUE(ThumbnailCacheOps::upsertThumbnail(*db_, record("/media/a.jpg", 256, 0, "/cache/first.jpg")));
    // This is the ON CONFLICT-target regression: if the conflict clause does
    // not match the UNIQUE constraint exactly, SQLite rejects the statement
    // and this second call returns false.
    ASSERT_TRUE(ThumbnailCacheOps::upsertThumbnail(*db_, record("/media/a.jpg", 256, 0, "/cache/second.jpg")));
    EXPECT_EQ(rowCount(), 1);
    auto got = ThumbnailCacheOps::getThumbnail(*db_, "/media/a.jpg", 256);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->cached_path, "/cache/second.jpg");

    // Access-time update is revision-keyed and must hit the row.
    ASSERT_TRUE(ThumbnailCacheOps::updateAccessTime(*db_, "/media/a.jpg", 256, 42));
    got = ThumbnailCacheOps::getThumbnail(*db_, "/media/a.jpg", 256);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->last_accessed_at, 42);
}

TEST_F(ThumbnailCacheOpsTest, EnsureTableMigratesPreRevisionTable)
{
    // Build the OLD shape by hand: no render_revision, two-column UNIQUE.
    {
        auto lease = db_->acquireSessionLease();
        auto &sess = lease.get();
        sess << "CREATE TABLE thumbnail_cache ("
                "id INTEGER PRIMARY KEY AUTOINCREMENT, source_path TEXT NOT NULL, cached_path TEXT NOT NULL, "
                "thumbnail_size INTEGER NOT NULL, file_size_bytes INTEGER NOT NULL, source_modified_at INTEGER NOT NULL, "
                "created_at INTEGER NOT NULL, last_accessed_at INTEGER NOT NULL, UNIQUE(source_path, thumbnail_size))",
            now;
        sess << "CREATE INDEX IF NOT EXISTS idx_thumbnail_source_path ON thumbnail_cache(source_path)", now;
        sess << "INSERT INTO thumbnail_cache(source_path, cached_path, thumbnail_size, file_size_bytes, "
                "source_modified_at, created_at, last_accessed_at) VALUES('/media/old.jpg', '/cache/old_128.jpg', 128, 1, 1, 1, 1)",
            now;
    }
    ASSERT_EQ(hasRenderRevisionColumn(), 0);
    ASSERT_EQ(rowCount(), 1);

    ASSERT_TRUE(ThumbnailCacheOps::ensureTable(*db_));

    EXPECT_EQ(hasRenderRevisionColumn(), 1) << "table was rebuilt with the new column";
    EXPECT_EQ(rowCount(), 0) << "old rows are discarded (cache invalidated)";

    // The new shape is fully functional: writes, revision-keyed reads, upsert.
    ASSERT_TRUE(ThumbnailCacheOps::upsertThumbnail(*db_, record("/media/new.jpg", 256, 0, "/cache/new_256_r0.jpg")));
    ASSERT_TRUE(ThumbnailCacheOps::upsertThumbnail(*db_, record("/media/new.jpg", 256, 0, "/cache/new_256_r0b.jpg")));
    EXPECT_EQ(rowCount(), 1);

    // Idempotent: a second ensureTable on the NEW shape keeps the rows.
    ASSERT_TRUE(ThumbnailCacheOps::ensureTable(*db_));
    EXPECT_EQ(rowCount(), 1);
    EXPECT_EQ(hasRenderRevisionColumn(), 1);
}

TEST_F(ThumbnailCacheOpsTest, CacheFileNameIsRevisionQualified)
{
    const std::string a0 = ThumbnailCacheOps::cacheFileName("/media/a.jpg", 256, 0);
    const std::string a1 = ThumbnailCacheOps::cacheFileName("/media/a.jpg", 256, 1);
    const std::string a0_512 = ThumbnailCacheOps::cacheFileName("/media/a.jpg", 512, 0);
    const std::string b0 = ThumbnailCacheOps::cacheFileName("/media/b.jpg", 256, 0);
    EXPECT_NE(a0, a1) << "two revisions of one file must not collide on disk";
    EXPECT_NE(a0, a0_512);
    EXPECT_NE(a0, b0);
    EXPECT_EQ(ThumbnailCacheOps::cacheFileName("/media/a.jpg", 256), a0) << "default revision is 0";
    EXPECT_NE(a0.find("_256_r0.jpg"), std::string::npos);
    EXPECT_NE(a1.find("_256_r1.jpg"), std::string::npos);
}
