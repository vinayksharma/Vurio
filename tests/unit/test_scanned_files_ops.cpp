#include <gtest/gtest.h>
#include <set>
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
            std::string db_path = "../tests/test_data/databases/test_scanned_files.sqlite";
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
            std::string db_path = "../tests/test_data/databases/test_count_verification.sqlite";
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

        TEST(ScannedFilesOpsTest, ErrorEscalationLogic)
        {
            std::string db_path = "../tests/test_data/databases/test_error_escalation.sqlite";
            std::remove(db_path.c_str());

            DatabaseManager dbm(db_path);
            ASSERT_TRUE(dbm.initialize());
            ASSERT_TRUE(ScannedFilesOps::ensureTable(dbm));

            // Create test file
            auto row = makeRow("/test/error_escalation.jpg");
            ASSERT_TRUE(ScannedFilesOps::upsert(dbm, row));

            // Test 1: First error should not be escalated
            EXPECT_TRUE(ScannedFilesOps::markProcessedWithEscalation(dbm, row.file_path, ServerMode::FAST, -1));
            auto fetched = ScannedFilesOps::getByPath(dbm, row.file_path);
            ASSERT_TRUE(fetched.has_value());
            EXPECT_EQ(fetched->processed_fast, -1); // Should remain -1, not escalated

            // Test 2: Second error should be escalated
            EXPECT_TRUE(ScannedFilesOps::markProcessedWithEscalation(dbm, row.file_path, ServerMode::FAST, -3));
            fetched = ScannedFilesOps::getByPath(dbm, row.file_path);
            ASSERT_TRUE(fetched.has_value());
            EXPECT_EQ(fetched->processed_fast, -103); // Should be escalated: -3 - 100 = -103

            // Test 3: Success after error should not be escalated
            EXPECT_TRUE(ScannedFilesOps::markProcessedWithEscalation(dbm, row.file_path, ServerMode::FAST, 2));
            fetched = ScannedFilesOps::getByPath(dbm, row.file_path);
            ASSERT_TRUE(fetched.has_value());
            EXPECT_EQ(fetched->processed_fast, 2); // Should be success state

            // Test 4: Error after success should not be escalated (back to initial error)
            EXPECT_TRUE(ScannedFilesOps::markProcessedWithEscalation(dbm, row.file_path, ServerMode::FAST, -4));
            fetched = ScannedFilesOps::getByPath(dbm, row.file_path);
            ASSERT_TRUE(fetched.has_value());
            EXPECT_EQ(fetched->processed_fast, -4); // Should be -4, not escalated

            // Test 5: Second error after success should be escalated
            EXPECT_TRUE(ScannedFilesOps::markProcessedWithEscalation(dbm, row.file_path, ServerMode::FAST, -5));
            fetched = ScannedFilesOps::getByPath(dbm, row.file_path);
            ASSERT_TRUE(fetched.has_value());
            EXPECT_EQ(fetched->processed_fast, -105); // Should be escalated: -5 - 100 = -105
        }

        TEST(ScannedFilesOpsTest, ErrorEscalationAllErrorCodes)
        {
            std::string db_path = "../tests/test_data/databases/test_all_error_codes.sqlite";
            std::remove(db_path.c_str());

            DatabaseManager dbm(db_path);
            ASSERT_TRUE(dbm.initialize());
            ASSERT_TRUE(ScannedFilesOps::ensureTable(dbm));

            // Test all error codes: -1, -3, -4, -5, -6
            std::vector<int> error_codes = {-1, -3, -4, -5, -6};
            std::vector<int> expected_escalated = {-101, -103, -104, -105, -106};

            for (size_t i = 0; i < error_codes.size(); ++i)
            {
                // Create test file for each error code
                auto row = makeRow("/test/error_" + std::to_string(error_codes[i]) + ".jpg");
                ASSERT_TRUE(ScannedFilesOps::upsert(dbm, row));

                // First error should not be escalated
                EXPECT_TRUE(ScannedFilesOps::markProcessedWithEscalation(dbm, row.file_path, ServerMode::FAST, error_codes[i]));
                auto fetched = ScannedFilesOps::getByPath(dbm, row.file_path);
                ASSERT_TRUE(fetched.has_value());
                EXPECT_EQ(fetched->processed_fast, error_codes[i]) << "First error should not be escalated for code " << error_codes[i];

                // Second error should be escalated
                EXPECT_TRUE(ScannedFilesOps::markProcessedWithEscalation(dbm, row.file_path, ServerMode::FAST, error_codes[i]));
                fetched = ScannedFilesOps::getByPath(dbm, row.file_path);
                ASSERT_TRUE(fetched.has_value());
                EXPECT_EQ(fetched->processed_fast, expected_escalated[i]) << "Second error should be escalated for code " << error_codes[i];
            }
        }

        TEST(ScannedFilesOpsTest, RetryLogicSQLQueries)
        {
            std::string db_path = "../tests/test_data/databases/test_retry_logic.sqlite";
            std::remove(db_path.c_str());

            DatabaseManager dbm(db_path);
            ASSERT_TRUE(dbm.initialize());
            ASSERT_TRUE(ScannedFilesOps::ensureTable(dbm));

            // Create test files with different status codes
            std::vector<std::pair<std::string, int>> test_files = {
                {"/test/unprocessed.jpg", 0},
                {"/test/success.jpg", 2},
                {"/test/error1.jpg", -1},
                {"/test/error3.jpg", -3},
                {"/test/error4.jpg", -4},
                {"/test/error5.jpg", -5},
                {"/test/error6.jpg", -6},
                {"/test/escalated101.jpg", -101},
                {"/test/escalated103.jpg", -103},
                {"/test/escalated104.jpg", -104},
                {"/test/escalated105.jpg", -105},
                {"/test/escalated106.jpg", -106}};

            // Insert all test files
            for (const auto &file : test_files)
            {
                auto row = makeRow(file.first);
                row.processed_fast = file.second;
                ASSERT_TRUE(ScannedFilesOps::upsert(dbm, row));
            }

            // Test listUnprocessed - should include unprocessed (0) and retryable errors (>= -100)
            auto unprocessed = ScannedFilesOps::listUnprocessed(dbm, ServerMode::FAST);

            // Should include: unprocessed (0), -1, -3, -4, -5, -6
            // Should NOT include: success (2), backpressure (-2), escalated errors (-101, -103, -104, -105, -106)
            EXPECT_EQ(unprocessed.size(), 6) << "Should include 6 retryable files (0, -1, -3, -4, -5, -6)";

            // Verify specific files are included
            std::set<std::string> unprocessed_paths;
            for (const auto &file : unprocessed)
            {
                unprocessed_paths.insert(file.file_path);
            }

            EXPECT_TRUE(unprocessed_paths.count("/test/unprocessed.jpg")) << "Unprocessed file should be included";
            EXPECT_TRUE(unprocessed_paths.count("/test/error1.jpg")) << "Error -1 should be included";
            EXPECT_TRUE(unprocessed_paths.count("/test/error3.jpg")) << "Error -3 should be included";
            EXPECT_TRUE(unprocessed_paths.count("/test/error4.jpg")) << "Error -4 should be included";
            EXPECT_TRUE(unprocessed_paths.count("/test/error5.jpg")) << "Error -5 should be included";
            EXPECT_TRUE(unprocessed_paths.count("/test/error6.jpg")) << "Error -6 should be included";

            // Verify escalated files are NOT included
            EXPECT_FALSE(unprocessed_paths.count("/test/success.jpg")) << "Success file should not be included";
            EXPECT_FALSE(unprocessed_paths.count("/test/escalated101.jpg")) << "Escalated error -101 should not be included";
            EXPECT_FALSE(unprocessed_paths.count("/test/escalated103.jpg")) << "Escalated error -103 should not be included";
            EXPECT_FALSE(unprocessed_paths.count("/test/escalated104.jpg")) << "Escalated error -104 should not be included";
            EXPECT_FALSE(unprocessed_paths.count("/test/escalated105.jpg")) << "Escalated error -105 should not be included";
            EXPECT_FALSE(unprocessed_paths.count("/test/escalated106.jpg")) << "Escalated error -106 should not be included";
        }

        TEST(ScannedFilesOpsTest, BackpressureNotEscalated)
        {
            std::string db_path = "../tests/test_data/databases/test_backpressure.sqlite";
            std::remove(db_path.c_str());

            DatabaseManager dbm(db_path);
            ASSERT_TRUE(dbm.initialize());
            ASSERT_TRUE(ScannedFilesOps::ensureTable(dbm));

            // Create test file
            auto row = makeRow("/test/backpressure.jpg");
            ASSERT_TRUE(ScannedFilesOps::upsert(dbm, row));

            // Test that backpressure (-2) is not escalated (should use regular markProcessed)
            EXPECT_TRUE(ScannedFilesOps::markProcessed(dbm, row.file_path, ServerMode::FAST, -2));
            auto fetched = ScannedFilesOps::getByPath(dbm, row.file_path);
            ASSERT_TRUE(fetched.has_value());
            EXPECT_EQ(fetched->processed_fast, -2); // Should remain -2

            // Test that backpressure files are not included in retry logic
            auto unprocessed = ScannedFilesOps::listUnprocessed(dbm, ServerMode::FAST);
            bool found_backpressure = false;
            for (const auto &file : unprocessed)
            {
                if (file.file_path == "/test/backpressure.jpg")
                {
                    found_backpressure = true;
                    break;
                }
            }
            EXPECT_FALSE(found_backpressure) << "Backpressure files should not be included in retry logic";
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
