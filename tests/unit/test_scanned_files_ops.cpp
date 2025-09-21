#include <gtest/gtest.h>
#include "database/database_manager.hpp"
#include "database/scanned_files_ops.hpp"

namespace MediaDedup
{
    namespace Test
    {
        static ScannedFileRow makeRow(const std::string &path)
        {
            ScannedFileRow r;
            r.file_path = path;
            r.relative_path = "rel";
            r.share_name = "share";
            r.file_name = "name";
            r.file_metadata = "meta";
            r.processed_fast = 0;
            r.processed_balanced = 0;
            r.processed_quality = 0;
            r.links_fast = "";
            r.links_balanced = "";
            r.links_quality = "";
            r.is_network_file = false;
            return r;
        }

        TEST(ScannedFilesOpsTest, CrudAndProcessingStates)
        {
            std::string db_path = "test_scanned_files.sqlite";
            std::remove(db_path.c_str());

            DatabaseManager dbm(db_path);
            ASSERT_TRUE(dbm.initialize());

            // Ensure table
            ASSERT_TRUE(ScannedFilesOps::ensureTable(dbm));

            // Upsert and get
            auto row = makeRow("/tmp/a.jpg");
            ASSERT_TRUE(ScannedFilesOps::upsert(dbm, row));
            auto fetched = ScannedFilesOps::getByPath(dbm, row.file_path);
            ASSERT_TRUE(fetched.has_value());
            EXPECT_EQ(fetched->file_path, row.file_path);
            EXPECT_EQ(fetched->processed_fast, 0);

            // Mark processed with state codes
            EXPECT_TRUE(ScannedFilesOps::markProcessed(dbm, row.file_path, ServerMode::FAST, 2));
            fetched = ScannedFilesOps::getByPath(dbm, row.file_path);
            ASSERT_TRUE(fetched.has_value());
            EXPECT_EQ(fetched->processed_fast, 2);

            // Set links and get links
            std::vector<int> links{1, 2, 3};
            EXPECT_TRUE(ScannedFilesOps::setLinks(dbm, row.file_path, ServerMode::FAST, links));
            auto gotLinks = ScannedFilesOps::getLinks(dbm, row.file_path, ServerMode::FAST);
            EXPECT_EQ(gotLinks, links);

            // List unprocessed for another mode should include this row still
            auto notBalanced = ScannedFilesOps::listUnprocessed(dbm, ServerMode::BALANCED);
            bool found = false;
            for (const auto &it : notBalanced)
                if (it.file_path == row.file_path)
                    found = true;
            EXPECT_TRUE(found);

            // Test count functionality
            EXPECT_EQ(ScannedFilesOps::count(dbm), 1);

            // Add another file
            auto row2 = makeRow("/tmp/b.jpg");
            ASSERT_TRUE(ScannedFilesOps::upsert(dbm, row2));
            EXPECT_EQ(ScannedFilesOps::count(dbm), 2);

            // Remove one file
            EXPECT_TRUE(ScannedFilesOps::removeByPath(dbm, row.file_path));
            EXPECT_EQ(ScannedFilesOps::count(dbm), 1);

            // Remove the other file
            EXPECT_TRUE(ScannedFilesOps::removeByPath(dbm, row2.file_path));
            EXPECT_EQ(ScannedFilesOps::count(dbm), 0);
        }

        TEST(ScannedFilesOpsTest, CountMethodVerification)
        {
            std::string db_path = "test_count_verification.sqlite";
            std::remove(db_path.c_str());

            DatabaseManager dbm(db_path);
            ASSERT_TRUE(dbm.initialize());
            ASSERT_TRUE(ScannedFilesOps::ensureTable(dbm));

            // Test empty table
            EXPECT_EQ(ScannedFilesOps::count(dbm), 0);

            // Add files and verify count increments
            std::vector<ScannedFileRow> testFiles;
            for (int i = 0; i < 5; ++i)
            {
                ScannedFileRow row = makeRow("/test/file" + std::to_string(i) + ".jpg");
                testFiles.push_back(row);
                ASSERT_TRUE(ScannedFilesOps::upsert(dbm, row));
                EXPECT_EQ(ScannedFilesOps::count(dbm), i + 1);
            }

            // Verify count matches listAll size
            auto allFiles = ScannedFilesOps::listAll(dbm);
            EXPECT_EQ(ScannedFilesOps::count(dbm), static_cast<int>(allFiles.size()));

            // Remove files and verify count decrements
            for (int i = 0; i < 5; ++i)
            {
                ASSERT_TRUE(ScannedFilesOps::removeByPath(dbm, testFiles[i].file_path));
                EXPECT_EQ(ScannedFilesOps::count(dbm), 4 - i);
            }

            // Verify empty table again
            EXPECT_EQ(ScannedFilesOps::count(dbm), 0);
        }
    }
}

#if !defined(ALL_UNIT_TESTS)
int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
#endif
