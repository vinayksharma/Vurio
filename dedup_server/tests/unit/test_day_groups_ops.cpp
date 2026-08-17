#include <gtest/gtest.h>
#include <ctime>
#include <filesystem>
#include <sstream>
#include <vector>
#include <Poco/Data/Session.h>
#include <Poco/Data/Statement.h>
#include "database/database_manager.hpp"
#include "database/day_groups_ops.hpp"
#include "database/duplicate_groups_ops.hpp"
#include "database/scanned_files_ops.hpp"
#include "database/sql_constants.hpp"
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
    insertFile("/media/mtime.heic", std::nullopt,
               "{\"modifiedAt\": 1678881600000000000, \"sizeBytes\": 12345}");

    auto page = DayGroupsOps::getDayGroups(*db_manager_, 0, 10);

    EXPECT_EQ(page.total_days, 1);
    ASSERT_EQ(page.days.size(), 1u);
    EXPECT_EQ(page.days[0].day, "2023-03-15");
    ASSERT_EQ(page.days[0].files.size(), 1u);
    EXPECT_EQ(page.days[0].files[0].taken_time, "2023-03-15 12:00:00");
    EXPECT_EQ(page.days[0].files[0].size_bytes, 12345);
}

TEST_F(DayGroupsOpsTest, ExcludesVideoFiles)
{
    // Only photos are supported at this time: video files (previously scanned
    // rows may exist) never appear in the day view.
    insertFile("/media/photo.jpg", std::string("2023:03:15 12:00:00"));
    insertFile("/media/clip.mp4", std::string("2023:03:15 13:00:00"));
    insertFile("/media/clip.mov", std::string("2023:03:16 09:00:00"));

    auto page = DayGroupsOps::getDayGroups(*db_manager_, 0, 10);

    // The video-only day (03-16) disappears entirely; 03-15 keeps the photo.
    EXPECT_EQ(page.total_days, 1);
    ASSERT_EQ(page.days.size(), 1u);
    EXPECT_EQ(page.days[0].day, "2023-03-15");
    ASSERT_EQ(page.days[0].files.size(), 1u);
    EXPECT_EQ(page.days[0].files[0].file_path, "/media/photo.jpg");
}

// DB-1 (backend_optimization_plan.md): the day list and day count must be
// served from the taken_day partial index (whose predicate excludes video
// rows via the is_video generated column), not by sweeping scanned_files and
// evaluating LIKE per row — that sweep cost 536-733 ms/page on the live DB
// and was the root cause of the session-pool contention.
TEST_F(DayGroupsOpsTest, DayQueriesAreServedByTheTakenDayIndex)
{
    insertFile("/media/plan.jpg", std::string("2024:06:01 10:00:00"));

    auto planFor = [&](std::string_view sql_text) {
        // Poco insists on binds for '?' even under EXPLAIN — substitute
        // literals (these queries only carry LIMIT/OFFSET placeholders).
        std::string sql(sql_text);
        for (std::string::size_type pos; (pos = sql.find('?')) != std::string::npos;)
            sql.replace(pos, 1, "1");

        auto lease = db_manager_->acquireSessionLease();
        Poco::Data::Session &sess = lease.get();
        std::vector<int> ids, parents, unused;
        std::vector<std::string> details;
        Poco::Data::Statement stmt(sess);
        stmt << "EXPLAIN QUERY PLAN " + sql,
            Poco::Data::Keywords::into(ids), Poco::Data::Keywords::into(parents),
            Poco::Data::Keywords::into(unused), Poco::Data::Keywords::into(details),
            Poco::Data::Keywords::now;
        std::ostringstream all;
        for (const auto &d : details)
            all << d << "\n";
        return all.str();
    };

    // Count and day-list queries: the scanned_files access must go through
    // the partial index (video rows are absent from it by predicate).
    for (std::string_view q : {SQL::kSelectScanDaysCount, SQL::kSelectScanDaysWithPagination})
    {
        // Strip trailing bind-parameter clauses for EXPLAIN on the paged query
        // by binding is unnecessary: EXPLAIN works with unbound parameters.
        std::string plan = planFor(q);
        EXPECT_NE(plan.find("idx_scanned_files_taken_day"), std::string::npos)
            << "day query not served by the partial index. Plan:\n" << plan;
    }
}

// DB-1: an existing database carries the pre-is_video index definition;
// ensureTable must detect it by text and rebuild it with the new predicate.
TEST_F(DayGroupsOpsTest, EnsureTableUpgradesLegacyTakenDayIndex)
{
    auto indexSql = [&]() {
        auto lease = db_manager_->acquireSessionLease();
        Poco::Data::Session &sess = lease.get();
        std::string sql;
        Poco::Data::Statement stmt(sess);
        stmt << "SELECT COALESCE(sql,'') FROM sqlite_master WHERE type='index' AND name='idx_scanned_files_taken_day'",
            Poco::Data::Keywords::into(sql), Poco::Data::Keywords::now;
        return sql;
    };

    // Simulate the legacy index (no is_video in the predicate).
    {
        auto lease = db_manager_->acquireSessionLease();
        Poco::Data::Session &sess = lease.get();
        Poco::Data::Statement drop(sess);
        drop << "DROP INDEX idx_scanned_files_taken_day", Poco::Data::Keywords::now;
        Poco::Data::Statement create(sess);
        create << "CREATE INDEX idx_scanned_files_taken_day ON scanned_files(taken_day) "
                  "WHERE processed >= 0 AND processed <> 3",
            Poco::Data::Keywords::now;
    }
    ASSERT_EQ(indexSql().find("is_video"), std::string::npos) << "test setup: legacy index expected";

    // Re-running the normal startup path performs the one-time rebuild.
    ASSERT_TRUE(ScannedFilesOps::ensureTable(*db_manager_));
    EXPECT_NE(indexSql().find("is_video"), std::string::npos)
        << "ensureTable must rebuild the index with the is_video predicate";

    // And the upgraded index still serves the day view correctly.
    insertFile("/media/after.jpg", std::string("2024:06:02 10:00:00"));
    insertFile("/media/after.mp4", std::string("2024:06:02 11:00:00"));
    auto page = DayGroupsOps::getDayGroups(*db_manager_, 0, 10, 10);
    ASSERT_EQ(page.days.size(), 1u);
    EXPECT_EQ(page.days[0].total_files, 1) << "video must stay excluded after the rebuild";
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

// THUMB-1 (docs/todo_aspect_ratio_thumbnails.md §5.3, §10): the day view
// carries each file's display geometry, parsed from exif_resolution, on every
// query shape — anchor branch, pulled-member branch, paged getDayFiles, and
// search mode (which builds its own column list and must match).
TEST_F(DayGroupsOpsTest, CarriesDisplayGeometryFromExifResolution)
{
    int landscape_id = insertFile("/media/landscape.jpg", std::string("2024:06:01 09:00:00"));
    int portrait_id = insertFile("/media/portrait.jpg", std::string("2024:06:01 09:05:00"));
    int malformed_id = insertFile("/media/malformed.jpg", std::string("2024:06:01 09:10:00"));
    insertFile("/media/none.jpg", std::string("2024:06:01 09:15:00"));
    // updateExifMetadata rewrites the datetime too, so pass it along.
    ASSERT_TRUE(ScannedFilesOps::updateExifMetadata(*db_manager_, "/media/landscape.jpg", "2024:06:01 09:00:00", "", "3000x2000"));
    ASSERT_TRUE(ScannedFilesOps::updateExifMetadata(*db_manager_, "/media/portrait.jpg", "2024:06:01 09:05:00", "", "2000x3000"));
    ASSERT_TRUE(ScannedFilesOps::updateExifMetadata(*db_manager_, "/media/malformed.jpg", "2024:06:01 09:10:00", "", "garbage"));

    // Anchor branch (getDayGroups, unfiltered)
    auto page = DayGroupsOps::getDayGroups(*db_manager_, 0, 10);
    ASSERT_EQ(page.days.size(), 1u);
    ASSERT_EQ(page.days[0].files.size(), 4u);
    EXPECT_EQ(page.days[0].files[0].display_width, 3000);
    EXPECT_EQ(page.days[0].files[0].display_height, 2000);
    EXPECT_EQ(page.days[0].files[1].display_width, 2000);
    EXPECT_EQ(page.days[0].files[1].display_height, 3000);
    EXPECT_EQ(page.days[0].files[2].display_width, 0) << "malformed -> unknown";
    EXPECT_EQ(page.days[0].files[2].display_height, 0);
    EXPECT_EQ(page.days[0].files[3].display_width, 0) << "NULL -> unknown";
    EXPECT_EQ(page.days[0].files[3].display_height, 0);

    // Paged getDayFiles (same SQL, paged variant)
    auto paged = DayGroupsOps::getDayFiles(*db_manager_, "2024-06-01", 1, 1);
    ASSERT_TRUE(paged.ok);
    ASSERT_EQ(paged.files.size(), 1u);
    EXPECT_EQ(paged.files[0].file_path, "/media/portrait.jpg");
    EXPECT_EQ(paged.files[0].display_width, 2000);
    EXPECT_EQ(paged.files[0].display_height, 3000);

    // Search mode: its own column list must carry the same 11 columns.
    std::vector<int> match_ids{portrait_id, malformed_id};
    auto searched = DayGroupsOps::getDayGroups(*db_manager_, 0, 10, 0, &match_ids);
    ASSERT_EQ(searched.days.size(), 1u);
    ASSERT_EQ(searched.days[0].files.size(), 2u);
    EXPECT_EQ(searched.days[0].files[0].file_path, "/media/portrait.jpg");
    EXPECT_EQ(searched.days[0].files[0].display_width, 2000);
    EXPECT_EQ(searched.days[0].files[0].display_height, 3000);
    EXPECT_EQ(searched.days[0].files[1].display_width, 0);

    auto searched_files = DayGroupsOps::getDayFiles(*db_manager_, "2024-06-01", 0, 10, &match_ids);
    ASSERT_TRUE(searched_files.ok);
    ASSERT_EQ(searched_files.files.size(), 2u);
    EXPECT_EQ(searched_files.files[0].display_width, 2000);
    EXPECT_EQ(searched_files.files[0].display_height, 3000);

    // Pulled-member branch: a non-representative member of a group whose
    // representative is on this day gets its OWN geometry, not the rep's.
    int other_day_id = insertFile("/media/otherday.jpg", std::string("2024:05:01 09:00:00"));
    ASSERT_TRUE(ScannedFilesOps::updateExifMetadata(*db_manager_, "/media/otherday.jpg", "2024:05:01 09:00:00", "", "1000x4000"));
    int group_id = DuplicateGroupsOps::createGroup(*db_manager_, "exact", landscape_id, "/media/landscape.jpg",
                                                   100, "2024-06-01 09:00:00", 0.98);
    ASSERT_GT(group_id, 0);
    ASSERT_TRUE(DuplicateGroupsOps::addMember(*db_manager_, group_id, landscape_id, "/media/landscape.jpg",
                                              1.0, 100, "2024-06-01 09:00:00", true));
    ASSERT_TRUE(DuplicateGroupsOps::addMember(*db_manager_, group_id, other_day_id, "/media/otherday.jpg",
                                              0.9, 100, "2024-05-01 09:00:00", false));

    auto grouped = DayGroupsOps::getDayGroups(*db_manager_, 0, 10);
    ASSERT_EQ(grouped.days.size(), 1u) << "the member is pulled onto the representative's day";
    ASSERT_EQ(grouped.days[0].files.size(), 5u);
    EXPECT_EQ(grouped.days[0].files[0].file_path, "/media/landscape.jpg");
    EXPECT_EQ(grouped.days[0].files[0].display_width, 3000);
    EXPECT_EQ(grouped.days[0].files[1].file_path, "/media/otherday.jpg");
    EXPECT_EQ(grouped.days[0].files[1].display_width, 1000);
    EXPECT_EQ(grouped.days[0].files[1].display_height, 4000);
}
