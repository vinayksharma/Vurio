#include <gtest/gtest.h>
#include <set>
#include "database/database_manager.hpp"
#include "database/scanned_files_ops.hpp"
#include "database/user_settings_ops.hpp"

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
            r.location_key = "mediaLocation:test123"; // Test location key
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

            // Test count functionality (using unfiltered count for backward compatibility)
            EXPECT_EQ(ScannedFilesOps::countAll(dbm), 1);

            // Add another file
            auto row2 = makeRow("/tmp/b.jpg");
            ASSERT_TRUE(ScannedFilesOps::upsert(dbm, row2));
            EXPECT_EQ(ScannedFilesOps::countAll(dbm), 2);

            // Remove one file
            EXPECT_TRUE(ScannedFilesOps::removeByPath(dbm, row.file_path));
            EXPECT_EQ(ScannedFilesOps::countAll(dbm), 1);

            // Remove the other file
            EXPECT_TRUE(ScannedFilesOps::removeByPath(dbm, row2.file_path));
            EXPECT_EQ(ScannedFilesOps::countAll(dbm), 0);
        }

        TEST(ScannedFilesOpsTest, CountMethodVerification)
        {
            std::string db_path = "../tests/test_data/databases/test_count_verification.sqlite";
            std::remove(db_path.c_str());

            DatabaseManager dbm(db_path);
            ASSERT_TRUE(dbm.initialize());
            ASSERT_TRUE(ScannedFilesOps::ensureTable(dbm));

            // Test empty table
            EXPECT_EQ(ScannedFilesOps::countAll(dbm), 0);

            // Add files and verify count increments
            std::vector<ScannedFileRow> testFiles;
            for (int i = 0; i < 5; ++i)
            {
                ScannedFileRow row = makeRow("/test/file" + std::to_string(i) + ".jpg");
                testFiles.push_back(row);
                ASSERT_TRUE(ScannedFilesOps::upsert(dbm, row));
                EXPECT_EQ(ScannedFilesOps::countAll(dbm), i + 1);
            }

            // Verify count matches listAll size
            auto allFiles = ScannedFilesOps::listAll(dbm);
            EXPECT_EQ(ScannedFilesOps::countAll(dbm), static_cast<int>(allFiles.size()));

            // Remove files and verify count decrements
            for (int i = 0; i < 5; ++i)
            {
                ASSERT_TRUE(ScannedFilesOps::removeByPath(dbm, testFiles[i].file_path));
                EXPECT_EQ(ScannedFilesOps::countAll(dbm), 4 - i);
            }

            // Verify empty table again
            EXPECT_EQ(ScannedFilesOps::countAll(dbm), 0);
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

        TEST(ScannedFilesOpsTest, BackpressureIncludedInRetry)
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

            // Test that backpressure files are now included in retry logic
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
            EXPECT_TRUE(found_backpressure) << "Backpressure files should now be included in retry logic";
        }

        TEST(ScannedFilesOpsTest, ErrorCountExcludesBackpressureAndQueued)
        {
            std::string db_path = "../tests/test_data/databases/test_error_count_exclusion.sqlite";
            std::remove(db_path.c_str());

            DatabaseManager dbm(db_path);
            ASSERT_TRUE(dbm.initialize());
            ASSERT_TRUE(ScannedFilesOps::ensureTable(dbm));

            // Ensure user_settings table for location filtering first
            // NOTE: location keys must start with "mediaLocation:" to be found by getRegisteredLocationKeys
            ASSERT_TRUE(UserSettingsOps::ensureTable(dbm));
            ASSERT_TRUE(UserSettingsOps::upsert(dbm, "mediaLocation:test123", "/test"));

            // Create files with different error statuses and proper location_key
            auto row_error = makeRow("/test/error.jpg");
            row_error.processed_fast = -1; // Real error
            row_error.location_key = "mediaLocation:test123";
            ASSERT_TRUE(ScannedFilesOps::upsert(dbm, row_error));

            auto row_backpressure = makeRow("/test/backpressure.jpg");
            row_backpressure.processed_fast = -2; // Backpressure (should NOT be counted as error)
            row_backpressure.location_key = "mediaLocation:test123";
            ASSERT_TRUE(ScannedFilesOps::upsert(dbm, row_backpressure));

            auto row_queued = makeRow("/test/queued.jpg");
            row_queued.processed_fast = -99; // Queued (should NOT be counted as error)
            row_queued.location_key = "mediaLocation:test123";
            ASSERT_TRUE(ScannedFilesOps::upsert(dbm, row_queued));

            auto row_escalated = makeRow("/test/escalated.jpg");
            row_escalated.processed_fast = -101; // Escalated error (SHOULD be counted as error)
            row_escalated.location_key = "mediaLocation:test123";
            ASSERT_TRUE(ScannedFilesOps::upsert(dbm, row_escalated));

            auto row_file_access = makeRow("/test/file_access.jpg");
            row_file_access.processed_fast = -3; // File access error (SHOULD be counted as error)
            row_file_access.location_key = "mediaLocation:test123";
            ASSERT_TRUE(ScannedFilesOps::upsert(dbm, row_file_access));

            // First test the unfiltered version (countErrorAll) to make sure the SQL itself works
            int error_count_all = ScannedFilesOps::countErrorAll(dbm, ServerMode::FAST);
            EXPECT_EQ(error_count_all, 3) << "Unfiltered error count should be 3: -1 (general), -3 (file access), -101 (escalated), excluding -2 (backpressure) and -99 (queued)";

            // Test queued count (unfiltered)
            int queued_count_all = ScannedFilesOps::countQueuedAll(dbm, ServerMode::FAST);
            EXPECT_EQ(queued_count_all, 1) << "Unfiltered queued count should be 1 for the -99 status file";

            // Verify location keys are registered
            auto registered_keys = ScannedFilesOps::getRegisteredLocationKeys(dbm);
            EXPECT_EQ(registered_keys.size(), 1) << "Should have 1 registered location";
            if (!registered_keys.empty()) {
                EXPECT_EQ(registered_keys[0], "mediaLocation:test123") << "Registered key should be 'mediaLocation:test123'";
            }

            // Now test the filtered versions (these use location_key filtering)
            int error_count = ScannedFilesOps::countError(dbm, ServerMode::FAST);
            EXPECT_EQ(error_count, 3) << "Filtered error count should be 3 (all files are in registered location)";

            int queued_count = ScannedFilesOps::countQueued(dbm, ServerMode::FAST);
            EXPECT_EQ(queued_count, 1) << "Filtered queued count should be 1";
        }
    }
}

TEST(ScannedFilesOpsTest, QueuedStatusCount)
{
    std::string db_path = "../tests/test_data/databases/test_queued_status_count.sqlite";
    std::remove(db_path.c_str());

    MediaDedup::DatabaseManager dbm(db_path);
    ASSERT_TRUE(dbm.initialize());
    ASSERT_TRUE(MediaDedup::ScannedFilesOps::ensureTable(dbm));

    // Create test files with different statuses
    auto file1 = MediaDedup::Test::makeRow("/test/queued_file1.jpg");
    auto file2 = MediaDedup::Test::makeRow("/test/queued_file2.jpg");
    auto file3 = MediaDedup::Test::makeRow("/test/processed_file.jpg");
    auto file4 = MediaDedup::Test::makeRow("/test/error_file.jpg");

    ASSERT_TRUE(MediaDedup::ScannedFilesOps::upsert(dbm, file1));
    ASSERT_TRUE(MediaDedup::ScannedFilesOps::upsert(dbm, file2));
    ASSERT_TRUE(MediaDedup::ScannedFilesOps::upsert(dbm, file3));
    ASSERT_TRUE(MediaDedup::ScannedFilesOps::upsert(dbm, file4));

    // Set different statuses
    EXPECT_TRUE(MediaDedup::ScannedFilesOps::markProcessed(dbm, file1.file_path, MediaDedup::ServerMode::FAST, -99)); // Queued
    EXPECT_TRUE(MediaDedup::ScannedFilesOps::markProcessed(dbm, file2.file_path, MediaDedup::ServerMode::FAST, -99)); // Queued
    EXPECT_TRUE(MediaDedup::ScannedFilesOps::markProcessed(dbm, file3.file_path, MediaDedup::ServerMode::FAST, 2));   // Processed
    EXPECT_TRUE(MediaDedup::ScannedFilesOps::markProcessed(dbm, file4.file_path, MediaDedup::ServerMode::FAST, -1));  // Error

    // Test queued count
    EXPECT_EQ(MediaDedup::ScannedFilesOps::countQueuedAll(dbm, MediaDedup::ServerMode::FAST), 2);
    EXPECT_EQ(MediaDedup::ScannedFilesOps::countQueuedAll(dbm, MediaDedup::ServerMode::BALANCED), 0);
    EXPECT_EQ(MediaDedup::ScannedFilesOps::countQueuedAll(dbm, MediaDedup::ServerMode::QUALITY), 0);

    // Test that queued files are included in unprocessed list
    auto unprocessed = MediaDedup::ScannedFilesOps::listUnprocessed(dbm, MediaDedup::ServerMode::FAST);
    EXPECT_EQ(unprocessed.size(), 3); // 2 queued + 1 error file
}

// TEST(ScannedFilesOpsTest, ResetAllErrors)
// {
//     std::string db_path = "../tests/test_data/databases/test_reset_errors.sqlite";
//     std::remove(db_path.c_str());

//     MediaDedup::DatabaseManager dbm(db_path);
//     ASSERT_TRUE(dbm.initialize());
//     ASSERT_TRUE(MediaDedup::ScannedFilesOps::ensureTable(dbm));

//     // Create test files with various error statuses
//     auto file1 = MediaDedup::Test::makeRow("/tmp/error1.jpg");
//     file1.processed_fast = -1;    // General error
//     file1.processed_balanced = -3; // File access error
//     file1.processed_quality = -101; // Escalated error
//     ASSERT_TRUE(MediaDedup::ScannedFilesOps::upsert(dbm, file1));

//     auto file2 = MediaDedup::Test::makeRow("/tmp/error2.jpg");
//     file2.processed_fast = -6;    // Cache error
//     file2.processed_balanced = 2;  // Successfully processed
//     file2.processed_quality = -2;  // Backpressure (should not be reset)
//     ASSERT_TRUE(MediaDedup::ScannedFilesOps::upsert(dbm, file2));

//     auto file3 = MediaDedup::Test::makeRow("/tmp/error3.jpg");
//     file3.processed_fast = 0;     // Unprocessed
//     file3.processed_balanced = -99; // Queued
//     file3.processed_quality = -106; // Escalated cache error
//     ASSERT_TRUE(MediaDedup::ScannedFilesOps::upsert(dbm, file3));

//     // Verify initial error counts
//     EXPECT_EQ(MediaDedup::ScannedFilesOps::countError(dbm, MediaDedup::ServerMode::FAST), 2);     // -1, -6
//     EXPECT_EQ(MediaDedup::ScannedFilesOps::countError(dbm, MediaDedup::ServerMode::BALANCED), 1); // -3
//     EXPECT_EQ(MediaDedup::ScannedFilesOps::countError(dbm, MediaDedup::ServerMode::QUALITY), 2);  // -101, -106

//     // Reset errors for FAST mode
//     int reset_count = MediaDedup::ScannedFilesOps::resetAllErrors(dbm, MediaDedup::ServerMode::FAST);
//     EXPECT_EQ(reset_count, 2); // Should reset 2 files with errors in FAST mode

//     // Verify FAST mode errors were reset
//     auto fetched1 = MediaDedup::ScannedFilesOps::getByPath(dbm, file1.file_path);
//     ASSERT_TRUE(fetched1.has_value());
//     EXPECT_EQ(fetched1->processed_fast, 0);     // Reset from -1 to 0
//     EXPECT_EQ(fetched1->processed_balanced, -3); // Unchanged
//     EXPECT_EQ(fetched1->processed_quality, -101); // Unchanged

//     auto fetched2 = MediaDedup::ScannedFilesOps::getByPath(dbm, file2.file_path);
//     ASSERT_TRUE(fetched2.has_value());
//     EXPECT_EQ(fetched2->processed_fast, 0);     // Reset from -6 to 0
//     EXPECT_EQ(fetched2->processed_balanced, 2);  // Unchanged
//     EXPECT_EQ(fetched2->processed_quality, -2);  // Unchanged

//     // Verify error counts after reset
//     EXPECT_EQ(MediaDedup::ScannedFilesOps::countError(dbm, MediaDedup::ServerMode::FAST), 0);     // All reset
//     EXPECT_EQ(MediaDedup::ScannedFilesOps::countError(dbm, MediaDedup::ServerMode::BALANCED), 1); // Unchanged
//     EXPECT_EQ(MediaDedup::ScannedFilesOps::countError(dbm, MediaDedup::ServerMode::QUALITY), 2);  // Unchanged

//     // Reset errors for BALANCED mode
//     reset_count = MediaDedup::ScannedFilesOps::resetAllErrors(dbm, MediaDedup::ServerMode::BALANCED);
//     EXPECT_EQ(reset_count, 1); // Should reset 1 file with error in BALANCED mode

//     // Verify BALANCED mode error was reset
//     fetched1 = MediaDedup::ScannedFilesOps::getByPath(dbm, file1.file_path);
//     ASSERT_TRUE(fetched1.has_value());
//     EXPECT_EQ(fetched1->processed_balanced, 0); // Reset from -3 to 0

//     // Reset errors for QUALITY mode
//     reset_count = MediaDedup::ScannedFilesOps::resetAllErrors(dbm, MediaDedup::ServerMode::QUALITY);
//     EXPECT_EQ(reset_count, 2); // Should reset 2 files with errors in QUALITY mode

//     // Verify QUALITY mode errors were reset
//     fetched1 = MediaDedup::ScannedFilesOps::getByPath(dbm, file1.file_path);
//     ASSERT_TRUE(fetched1.has_value());
//     EXPECT_EQ(fetched1->processed_quality, 0); // Reset from -101 to 0

//     auto fetched3 = MediaDedup::ScannedFilesOps::getByPath(dbm, file3.file_path);
//     ASSERT_TRUE(fetched3.has_value());
//     EXPECT_EQ(fetched3->processed_quality, 0); // Reset from -106 to 0

//     // Verify all error counts are now 0
//     EXPECT_EQ(MediaDedup::ScannedFilesOps::countError(dbm, MediaDedup::ServerMode::FAST), 0);
//     EXPECT_EQ(MediaDedup::ScannedFilesOps::countError(dbm, MediaDedup::ServerMode::BALANCED), 0);
//     EXPECT_EQ(MediaDedup::ScannedFilesOps::countError(dbm, MediaDedup::ServerMode::QUALITY), 0);

//     // Test reset with no errors (should return 0)
//     reset_count = MediaDedup::ScannedFilesOps::resetAllErrors(dbm, MediaDedup::ServerMode::FAST);
//     EXPECT_EQ(reset_count, 0);
// }

    TEST(ScannedFilesOpsTest, FilteredCountsByLocationKey)
    {
        std::string db_path = "../tests/test_data/databases/test_filtered_counts.sqlite";
        std::remove(db_path.c_str());

        MediaDedup::DatabaseManager dbm(db_path);
        ASSERT_TRUE(dbm.initialize());

        // Ensure tables exist
        ASSERT_TRUE(MediaDedup::ScannedFilesOps::ensureTable(dbm));
        
        // Create test data with different location keys
        auto row1 = MediaDedup::Test::makeRow("/tmp/test1.jpg");
        row1.location_key = "mediaLocation:abc123";
        row1.processed_fast = 2; // processed
        
        auto row2 = MediaDedup::Test::makeRow("/tmp/test2.jpg");
        row2.location_key = "mediaLocation:abc123";
        row2.processed_fast = -1; // error
        
        auto row3 = MediaDedup::Test::makeRow("/tmp/test3.jpg");
        row3.location_key = "mediaLocation:def456";
        row3.processed_fast = 0; // unprocessed
        
        auto row4 = MediaDedup::Test::makeRow("/tmp/test4.jpg");
        row4.location_key = "mediaLocation:xyz789"; // This location will not be registered
        row4.processed_fast = 2; // processed
        
        // Insert test data
        ASSERT_TRUE(MediaDedup::ScannedFilesOps::upsert(dbm, row1));
        ASSERT_TRUE(MediaDedup::ScannedFilesOps::upsert(dbm, row2));
        ASSERT_TRUE(MediaDedup::ScannedFilesOps::upsert(dbm, row3));
        ASSERT_TRUE(MediaDedup::ScannedFilesOps::upsert(dbm, row4));
        
        // Ensure user_settings table exists and register only two locations
        ASSERT_TRUE(MediaDedup::UserSettingsOps::ensureTable(dbm));
        bool upsert1 = MediaDedup::UserSettingsOps::upsert(dbm, "mediaLocation:abc123", "/tmp/location1");
        bool upsert2 = MediaDedup::UserSettingsOps::upsert(dbm, "mediaLocation:def456", "/tmp/location2");
        ASSERT_TRUE(upsert1);
        ASSERT_TRUE(upsert2);
        
        // Test filtered counts - should only count files from registered locations
        int total_count = MediaDedup::ScannedFilesOps::count(dbm);
        EXPECT_EQ(total_count, 3); // Only files from registered locations
        
        int processed_count = MediaDedup::ScannedFilesOps::countProcessed(dbm, MediaDedup::ServerMode::FAST);
        EXPECT_EQ(processed_count, 1); // Only row1 is processed and from registered location
        
        int error_count = MediaDedup::ScannedFilesOps::countError(dbm, MediaDedup::ServerMode::FAST);
        EXPECT_EQ(error_count, 1); // Only row2 has error and is from registered location
        
        // Test getRegisteredLocationKeys
        auto registered_keys = MediaDedup::ScannedFilesOps::getRegisteredLocationKeys(dbm);
        EXPECT_EQ(registered_keys.size(), 2);
        EXPECT_TRUE(std::find(registered_keys.begin(), registered_keys.end(), "mediaLocation:abc123") != registered_keys.end());
        EXPECT_TRUE(std::find(registered_keys.begin(), registered_keys.end(), "mediaLocation:def456") != registered_keys.end());
        
        // Test getLocationKey
        std::string location_key = MediaDedup::ScannedFilesOps::getLocationKey(dbm, "/tmp/test1.jpg");
        EXPECT_EQ(location_key, "mediaLocation:abc123");
        
        // Clean up
        std::remove(db_path.c_str());
}

#if !defined(ALL_UNIT_TESTS)
int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
#endif
