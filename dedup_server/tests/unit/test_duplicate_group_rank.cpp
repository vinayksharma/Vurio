/**
 * @file test_duplicate_group_rank.cpp
 * @brief getGroupRank must agree with the paginated BINNED display order.
 *
 * The client anchors its scroll position on a group id: after a re-cluster it
 * asks the server where that group sits now and puts the viewport back there.
 * That is only stable if the rank query and the ORDER BY behind the pages are
 * the same total order — including for groups that share a representative
 * capture date, which is exactly where a date-only ordering has no answer.
 */

#include <gtest/gtest.h>
#include "database/database_manager.hpp"
#include "database/duplicate_groups_ops.hpp"
#include "database/scanned_files_ops.hpp"
#include <filesystem>
#include <string>
#include <vector>

using namespace MediaDedup;

class DuplicateGroupRankTest : public ::testing::Test
{
protected:
    std::string test_db_path_;
    std::unique_ptr<DatabaseManager> db_;

    void SetUp() override
    {
        test_db_path_ = "/tmp/test_dup_rank_" + std::to_string(std::time(nullptr)) + ".db";
        std::filesystem::remove(test_db_path_);

        db_ = std::make_unique<DatabaseManager>(test_db_path_);
        ASSERT_TRUE(db_->initialize());
        ASSERT_TRUE(ScannedFilesOps::ensureTable(*db_));
        ASSERT_TRUE(DuplicateGroupsOps::ensureTables(*db_));
    }

    void TearDown() override
    {
        db_.reset();
        std::filesystem::remove(test_db_path_);
    }

    /// One group whose representative carries `created_date`.
    int makeGroup(const std::string &created_date, int seq)
    {
        std::string file_path = "/test/rank_" + std::to_string(seq) + ".jpg";

        ScannedFileRow file_row;
        file_row.file_path = file_path;
        file_row.relative_path = file_path;
        file_row.location_key = "test_location";
        file_row.file_name = file_path;
        ScannedFilesOps::upsert(*db_, file_row);

        auto inserted = ScannedFilesOps::getByPath(*db_, file_path);
        EXPECT_TRUE(inserted.has_value());
        int file_id = inserted.has_value() ? inserted->id : 0;

        int group_id = DuplicateGroupsOps::createGroup(
            *db_, "EMBEDDING", file_id, file_path, 1024, created_date, 0.95);
        EXPECT_GT(group_id, 0);
        DuplicateGroupsOps::addMember(*db_, group_id, file_id, file_path, 1.0, 1024, created_date, true);
        return group_id;
    }

    /// Group ids in the order the paginated query returns them.
    std::vector<int> displayOrder()
    {
        auto page = DuplicateGroupsOps::getGroupsWithMembers(*db_, 0, 1000, "");
        std::vector<int> ids;
        ids.reserve(page.groups.size());
        for (const auto &g : page.groups)
            ids.push_back(g.group.id);
        return ids;
    }
};

TEST_F(DuplicateGroupRankTest, RankMatchesDisplayOrder)
{
    // Deliberately unsorted insertion order, and three groups sharing
    // "2025-03-02" so the date-only comparison is ambiguous for them.
    makeGroup("2025-01-05", 0);
    makeGroup("2025-03-02", 1);
    makeGroup("2024-12-31", 2);
    makeGroup("2025-03-02", 3);
    makeGroup("2025-07-19", 4);
    makeGroup("2025-03-02", 5);

    const auto order = displayOrder();
    ASSERT_EQ(order.size(), 6u);

    for (std::size_t i = 0; i < order.size(); ++i)
    {
        EXPECT_EQ(DuplicateGroupsOps::getGroupRank(*db_, order[i]), static_cast<int>(i))
            << "group " << order[i] << " reports a rank that is not its display position";
    }
}

TEST_F(DuplicateGroupRankTest, RankTracksInsertionsAbove)
{
    const int anchor = makeGroup("2025-01-05", 0);
    makeGroup("2024-06-01", 1); // sorts below the anchor (older)
    ASSERT_EQ(DuplicateGroupsOps::getGroupRank(*db_, anchor), 0);

    // Two newer groups appear — the anchor slides down by exactly two rows,
    // which is the shift the client compensates for.
    makeGroup("2026-01-01", 2);
    makeGroup("2025-11-11", 3);
    EXPECT_EQ(DuplicateGroupsOps::getGroupRank(*db_, anchor), 2);

    const auto order = displayOrder();
    ASSERT_EQ(order.size(), 4u);
    EXPECT_EQ(order[2], anchor);
}

TEST_F(DuplicateGroupRankTest, MissingGroupHasNoRank)
{
    const int group_id = makeGroup("2025-01-05", 0);
    ASSERT_EQ(DuplicateGroupsOps::getGroupRank(*db_, group_id), 0);

    ASSERT_TRUE(DuplicateGroupsOps::deleteGroup(*db_, group_id));
    EXPECT_EQ(DuplicateGroupsOps::getGroupRank(*db_, group_id), -1)
        << "a deleted anchor must report absent, not position 0";
}
