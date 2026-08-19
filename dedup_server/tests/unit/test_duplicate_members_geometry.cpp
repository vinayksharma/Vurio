// THUMB-1 (docs/todo_aspect_ratio_thumbnails.md §5.3): the duplicates view
// carries each member's display geometry. Both member queries — the batched
// getGroupsWithMembers members_query and kSelectDuplicateMembersByGroup (which
// feeds OnDuplicateGroupEvent's DuplicateFile protos) — LEFT JOIN
// scanned_files for exif_resolution. (An orphaned member cannot exist by
// construction — duplicate_members.file_id is a cascading FK — so the LEFT is
// defensive; what is tested here is that unknown geometry stays 0/0.)

#include <gtest/gtest.h>
#include <Poco/Data/Session.h>
#include <Poco/Data/Statement.h>
#include <ctime>
#include <filesystem>
#include <string>

#include "database/database_manager.hpp"
#include "database/duplicate_groups_ops.hpp"
#include "database/scanned_files_ops.hpp"
#include "database/thumbnail_cache_ops.hpp"

using namespace MediaDedup;

class DuplicateMembersGeometryTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        db_path_ = (std::filesystem::temp_directory_path() /
                    ("mds_dup_members_geometry_" + std::to_string(std::time(nullptr)) + ".sqlite"))
                       .string();
        std::filesystem::remove(db_path_);
        db_ = std::make_unique<DatabaseManager>(db_path_);
        ASSERT_TRUE(db_->initialize());
        ASSERT_TRUE(ScannedFilesOps::ensureTable(*db_));
        ASSERT_TRUE(DuplicateGroupsOps::ensureTables(*db_));
        ASSERT_TRUE(ThumbnailCacheOps::ensureTable(*db_));
    }
    void TearDown() override
    {
        db_.reset();
        std::filesystem::remove(db_path_);
    }

    int insertFile(const std::string &path, const std::string &exif_resolution)
    {
        ScannedFileRow row;
        row.file_path = path;
        row.file_name = std::filesystem::path(path).filename().string();
        row.location_key = "mediaLocation:test";
        row.processed = 2;
        EXPECT_TRUE(ScannedFilesOps::upsert(*db_, row));
        auto stored = ScannedFilesOps::getByPath(*db_, path);
        EXPECT_TRUE(stored.has_value());
        if (!exif_resolution.empty())
        {
            // EXIF-1: updateExifMetadata now derives resolution from ExifInfo
            // width/height; this test needs raw strings, so write the column.
            auto lease = db_->acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            std::string res = exif_resolution;
            std::string p = path;
            sess << "UPDATE scanned_files SET exif_resolution = ? WHERE file_path = ?",
                Poco::Data::Keywords::use(res), Poco::Data::Keywords::use(p), Poco::Data::Keywords::now;
        }
        return stored ? stored->id : 0;
    }

    std::string db_path_;
    std::unique_ptr<DatabaseManager> db_;
};

TEST_F(DuplicateMembersGeometryTest, BothMemberQueriesCarryDisplayGeometry)
{
    int rep_id = insertFile("/media/rep.jpg", "4032x3024");
    int dup_id = insertFile("/media/dup.jpg", "3024x4032");
    int nores_id = insertFile("/media/nores.jpg", "");

    int group_id = DuplicateGroupsOps::createGroup(*db_, "EMBEDDING", rep_id, "/media/rep.jpg",
                                                   100, "2024-06-01 09:00:00", 0.95);
    ASSERT_GT(group_id, 0);
    ASSERT_TRUE(DuplicateGroupsOps::addMember(*db_, group_id, rep_id, "/media/rep.jpg", 1.0, 100, "2024-06-01 09:00:00", true));
    ASSERT_TRUE(DuplicateGroupsOps::addMember(*db_, group_id, dup_id, "/media/dup.jpg", 0.99, 100, "2024-06-01 09:05:00", false));
    ASSERT_TRUE(DuplicateGroupsOps::addMember(*db_, group_id, nores_id, "/media/nores.jpg", 0.98, 100, "2024-06-01 09:06:00", false));

    ThumbnailCacheRecord thumb;
    thumb.source_path = "/media/rep.jpg";
    thumb.cached_path = "/cache/rep_256.jpg";
    thumb.thumbnail_size = 256;
    ASSERT_TRUE(ThumbnailCacheOps::upsertThumbnail(*db_, thumb));

    // Batched page query
    auto page = DuplicateGroupsOps::getGroupsWithMembers(*db_, 0, 10, "");
    ASSERT_EQ(page.groups.size(), 1u);
    const auto &members = page.groups[0].members;
    ASSERT_EQ(members.size(), 3u);
    EXPECT_EQ(members[0].file_path, "/media/rep.jpg");
    EXPECT_EQ(members[0].display_width, 4032);
    EXPECT_EQ(members[0].display_height, 3024);
    EXPECT_EQ(members[0].cached_thumbnail_path, "/cache/rep_256.jpg");
    EXPECT_EQ(members[1].file_path, "/media/dup.jpg");
    EXPECT_EQ(members[1].display_width, 3024);
    EXPECT_EQ(members[1].display_height, 4032);
    EXPECT_EQ(members[2].file_path, "/media/nores.jpg");
    EXPECT_EQ(members[2].display_width, 0) << "NULL exif_resolution -> unknown";
    EXPECT_EQ(members[2].display_height, 0);

    // Per-group query (event push path)
    auto by_group = DuplicateGroupsOps::getMembersByGroup(*db_, group_id);
    ASSERT_EQ(by_group.size(), 3u);
    EXPECT_EQ(by_group[0].file_path, "/media/rep.jpg");
    EXPECT_EQ(by_group[0].display_width, 4032);
    EXPECT_EQ(by_group[0].display_height, 3024);
    EXPECT_EQ(by_group[0].cached_thumbnail_path, "/cache/rep_256.jpg");
    EXPECT_EQ(by_group[1].display_width, 3024);
    EXPECT_EQ(by_group[1].display_height, 4032);
    EXPECT_EQ(by_group[2].display_width, 0);
    EXPECT_EQ(by_group[2].display_height, 0);
}
