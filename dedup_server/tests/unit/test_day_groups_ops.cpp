#include <gtest/gtest.h>
#include <ctime>
#include <filesystem>
#include "database/database_manager.hpp"
#include "database/day_groups_ops.hpp"
#include "database/duplicate_groups_ops.hpp"
#include "database/scanned_files_ops.hpp"
#include "database/thumbnail_cache_ops.hpp"

using namespace MediaDedup;

class DayGroupsOpsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        db_path_ = (std::filesystem::temp_directory_path() / "mds_day_groups_test.sqlite").string();
        std::remove(db_path_.c_str());

        db_manager_ = std::make_shared<DatabaseManager>(db_path_);
        ASSERT_TRUE(db_manager_->initialize());

        ASSERT_TRUE(ScannedFilesOps::ensureTable(*db_manager_));
        ASSERT_TRUE(DuplicateGroupsOps::ensureTables(*db_manager_));
        ASSERT_TRUE(ThumbnailCacheOps::ensureTable(*db_manager_));
    }

    void TearDown() override
    {
        db_manager_.reset();
        if (std::filesystem::exists(db_path_))
        {
            std::filesystem::remove(db_path_);
        }
    }

    // Inserts a scanned file and returns its row id.
    int insertFile(const std::string &path,
                   const std::optional<std::string> &exif_datetime,
                   const std::string &file_metadata = "",
                   int processed = 2)
    {
        ScannedFileRow row;
        row.file_path = path;
        row.file_name = std::filesystem::path(path).filename().string();
        row.file_metadata = file_metadata;
        row.processed = processed;
        row.location_key = "mediaLocation:test";
        row.exif_datetime = exif_datetime;
        EXPECT_TRUE(ScannedFilesOps::upsert(*db_manager_, row));

        auto stored = ScannedFilesOps::getByPath(*db_manager_, path);
        EXPECT_TRUE(stored.has_value());
        return stored ? stored->id : 0;
    }

    std::string db_path_;
    std::shared_ptr<DatabaseManager> db_manager_;
};

TEST_F(DayGroupsOpsTest, GroupsByExifDayNewestFirstAscendingWithinDay)
{
    // Insert out of order within the newer day to prove ascending time sort.
    insertFile("/media/b.jpg", std::string("2024:06:01 10:00:00"));
    insertFile("/media/a.jpg", std::string("2024:06:01 09:00:00"));
    insertFile("/media/c.jpg", std::string("2024:05:30 12:00:00"));

    auto page = DayGroupsOps::getDayGroups(*db_manager_, 0, 10);

    EXPECT_EQ(page.total_days, 2);
    ASSERT_EQ(page.days.size(), 2u);

    EXPECT_EQ(page.days[0].day, "2024-06-01");
    ASSERT_EQ(page.days[0].files.size(), 2u);
    EXPECT_EQ(page.days[0].files[0].file_path, "/media/a.jpg");
    EXPECT_EQ(page.days[0].files[0].taken_time, "2024-06-01 09:00:00");
    EXPECT_EQ(page.days[0].files[1].file_path, "/media/b.jpg");
    EXPECT_EQ(page.days[0].files[1].taken_time, "2024-06-01 10:00:00");

    EXPECT_EQ(page.days[1].day, "2024-05-30");
    ASSERT_EQ(page.days[1].files.size(), 1u);
    EXPECT_EQ(page.days[1].files[0].file_path, "/media/c.jpg");
}

TEST_F(DayGroupsOpsTest, FallsBackToFileMtimeFromMetadata)
{
    // 2023-03-15 12:00:00 UTC = 1678881600s; modifiedAt is stored as
    // nanoseconds since the Unix epoch (file_time_type ticks on Apple libc++).
    insertFile("/media/video.mp4", std::nullopt,
               "{\"modifiedAt\": 1678881600000000000, \"sizeBytes\": 12345}");

    auto page = DayGroupsOps::getDayGroups(*db_manager_, 0, 10);

    EXPECT_EQ(page.total_days, 1);
    ASSERT_EQ(page.days.size(), 1u);
    EXPECT_EQ(page.days[0].day, "2023-03-15");
    ASSERT_EQ(page.days[0].files.size(), 1u);
    EXPECT_EQ(page.days[0].files[0].taken_time, "2023-03-15 12:00:00");
    EXPECT_EQ(page.days[0].files[0].size_bytes, 12345);
}

TEST_F(DayGroupsOpsTest, GarbageExifFallsBackToMetadata)
{
    insertFile("/media/broken.jpg", std::string("0000:00:00 00:00:00"),
               "{\"modifiedAt\": 1678881600000000000}");

    auto page = DayGroupsOps::getDayGroups(*db_manager_, 0, 10);

    ASSERT_EQ(page.days.size(), 1u);
    EXPECT_EQ(page.days[0].day, "2023-03-15");
}

TEST_F(DayGroupsOpsTest, FallsBackToCreatedAtWhenNoExifOrMetadata)
{
    insertFile("/media/unknown.jpg", std::nullopt);

    // created_at defaults to CURRENT_TIMESTAMP (UTC).
    std::time_t now = std::time(nullptr);
    char expected_day[11];
    std::strftime(expected_day, sizeof(expected_day), "%Y-%m-%d", std::gmtime(&now));

    auto page = DayGroupsOps::getDayGroups(*db_manager_, 0, 10);

    EXPECT_EQ(page.total_days, 1);
    ASSERT_EQ(page.days.size(), 1u);
    EXPECT_EQ(page.days[0].day, expected_day);
    ASSERT_EQ(page.days[0].files.size(), 1u);
    EXPECT_FALSE(page.days[0].files[0].taken_time.empty());
}

TEST_F(DayGroupsOpsTest, CarriesDuplicateGroupMembershipAndThumbnail)
{
    int rep_id = insertFile("/media/rep.jpg", std::string("2024:06:01 09:00:00"));
    int dup_id = insertFile("/media/dup.jpg", std::string("2024:06:01 09:05:00"));
    insertFile("/media/unique.jpg", std::string("2024:06:01 11:00:00"));

    int group_id = DuplicateGroupsOps::createGroup(*db_manager_, "exact", rep_id, "/media/rep.jpg",
                                                   100, "2024-06-01 09:00:00", 0.98);
    ASSERT_GT(group_id, 0);
    ASSERT_TRUE(DuplicateGroupsOps::addMember(*db_manager_, group_id, rep_id, "/media/rep.jpg",
                                              1.0, 100, "2024-06-01 09:00:00", true));
    ASSERT_TRUE(DuplicateGroupsOps::addMember(*db_manager_, group_id, dup_id, "/media/dup.jpg",
                                              0.99, 100, "2024-06-01 09:05:00", false));

    ThumbnailCacheRecord thumb;
    thumb.source_path = "/media/rep.jpg";
    thumb.cached_path = "/cache/rep_256.jpg";
    thumb.thumbnail_size = 256;
    ASSERT_TRUE(ThumbnailCacheOps::upsertThumbnail(*db_manager_, thumb));

    auto page = DayGroupsOps::getDayGroups(*db_manager_, 0, 10);

    ASSERT_EQ(page.days.size(), 1u);
    ASSERT_EQ(page.days[0].files.size(), 3u);

    const auto &rep = page.days[0].files[0];
    EXPECT_EQ(rep.file_path, "/media/rep.jpg");
    EXPECT_EQ(rep.group_id, group_id);
    EXPECT_TRUE(rep.is_representative);
    EXPECT_DOUBLE_EQ(rep.similarity_score, 1.0);
    EXPECT_EQ(rep.cached_thumbnail_path, "/cache/rep_256.jpg");

    const auto &dup = page.days[0].files[1];
    EXPECT_EQ(dup.file_path, "/media/dup.jpg");
    EXPECT_EQ(dup.group_id, group_id);
    EXPECT_FALSE(dup.is_representative);
    EXPECT_DOUBLE_EQ(dup.similarity_score, 0.99);

    const auto &unique = page.days[0].files[2];
    EXPECT_EQ(unique.file_path, "/media/unique.jpg");
    EXPECT_EQ(unique.group_id, 0);
    EXPECT_TRUE(unique.cached_thumbnail_path.empty());
}

TEST_F(DayGroupsOpsTest, ExcludesIgnoredAndErroredFiles)
{
    insertFile("/media/good.jpg", std::string("2024:06:01 09:00:00"));
    insertFile("/media/ignored.jpg", std::string("2024:06:01 09:10:00"), "", 3);
    insertFile("/media/errored.jpg", std::string("2024:06:01 09:20:00"), "", -1);

    auto page = DayGroupsOps::getDayGroups(*db_manager_, 0, 10);

    EXPECT_EQ(page.total_days, 1);
    ASSERT_EQ(page.days.size(), 1u);
    ASSERT_EQ(page.days[0].files.size(), 1u);
    EXPECT_EQ(page.days[0].files[0].file_path, "/media/good.jpg");
}

TEST_F(DayGroupsOpsTest, PaginatesOverDays)
{
    insertFile("/media/d1.jpg", std::string("2024:06:03 09:00:00"));
    insertFile("/media/d2.jpg", std::string("2024:06:02 09:00:00"));
    insertFile("/media/d3.jpg", std::string("2024:06:01 09:00:00"));

    auto first = DayGroupsOps::getDayGroups(*db_manager_, 0, 2);
    EXPECT_EQ(first.total_days, 3);
    ASSERT_EQ(first.days.size(), 2u);
    EXPECT_EQ(first.days[0].day, "2024-06-03");
    EXPECT_EQ(first.days[1].day, "2024-06-02");

    auto second = DayGroupsOps::getDayGroups(*db_manager_, 2, 2);
    EXPECT_EQ(second.total_days, 3);
    ASSERT_EQ(second.days.size(), 1u);
    EXPECT_EQ(second.days[0].day, "2024-06-01");
}
