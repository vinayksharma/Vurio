#include <gtest/gtest.h>
#include <filesystem>
#include "database/database_manager.hpp"
#include "database/processing_errors_ops.hpp"
#include "database/scanned_files_ops.hpp"
#include "config/config_enums.hpp"
#include "test_utils.hpp"

namespace MediaDedup
{
    namespace Test
    {
        static std::string testDbPath(const std::string& name) {
            std::filesystem::path dir = std::filesystem::path(TestUtils::getTestDataPath()) / "databases" / "processing_errors_ops";
            std::filesystem::create_directories(dir);
            return (dir / name).string();
        }

        static void removeDbAndWal(const std::string& db_path) {
            std::remove(db_path.c_str());
            std::remove((db_path + "-shm").c_str());
            std::remove((db_path + "-wal").c_str());
        }

        TEST(ProcessingErrorsOpsTest, EnsureTableCreation)
        {
            std::string db_path = testDbPath("test_processing_errors.sqlite");
            removeDbAndWal(db_path);

            DatabaseManager dbm(db_path);
            ASSERT_TRUE(dbm.initialize());

            // Ensure table creation
            ASSERT_TRUE(ProcessingErrorsOps::ensureTable(dbm));

            // Table creation is validated by subsequent tests that successfully insert data
        }

        TEST(ProcessingErrorsOpsTest, InsertErrorBasic)
        {
            std::string db_path = testDbPath("test_processing_errors_insert.sqlite");
            removeDbAndWal(db_path);

            DatabaseManager dbm(db_path);
            ASSERT_TRUE(dbm.initialize());
            ASSERT_TRUE(ProcessingErrorsOps::ensureTable(dbm));

            // Insert an error
            bool result = ProcessingErrorsOps::insertError(
                dbm,
                "/path/to/test/file.jpg",
                ServerMode::EMBEDDING,
                -1,
                "Test error message",
                "OpenCV");

            EXPECT_TRUE(result);
        }

        TEST(ProcessingErrorsOpsTest, InsertMultipleErrors)
        {
            std::string db_path = testDbPath("test_processing_errors_multiple.sqlite");
            removeDbAndWal(db_path);

            DatabaseManager dbm(db_path);
            ASSERT_TRUE(dbm.initialize());
            ASSERT_TRUE(ProcessingErrorsOps::ensureTable(dbm));

            // Insert multiple errors for different files
            EXPECT_TRUE(ProcessingErrorsOps::insertError(
                dbm, "/file1.jpg", ServerMode::EMBEDDING, -1, "Error 1", "ImageMagick"));

            EXPECT_TRUE(ProcessingErrorsOps::insertError(
                dbm, "/file2.jpg", ServerMode::EMBEDDING, -3, "File access error", "FileSystem"));

            EXPECT_TRUE(ProcessingErrorsOps::insertError(
                dbm, "/file3.jpg", ServerMode::EMBEDDING, -4, "Out of memory", "Memory"));

            // Insert multiple errors for the same file (allowed)
            EXPECT_TRUE(ProcessingErrorsOps::insertError(
                dbm, "/file1.jpg", ServerMode::EMBEDDING, -101, "Escalated error", "ImageMagick"));
        }

        TEST(ProcessingErrorsOpsTest, InsertErrorAllModes)
        {
            std::string db_path = testDbPath("test_processing_errors_modes.sqlite");
            removeDbAndWal(db_path);

            DatabaseManager dbm(db_path);
            ASSERT_TRUE(dbm.initialize());
            ASSERT_TRUE(ProcessingErrorsOps::ensureTable(dbm));

            // Test all server modes
            EXPECT_TRUE(ProcessingErrorsOps::insertError(
                dbm, "/test.jpg", ServerMode::EMBEDDING, -1, "Fast mode error", "General"));

            EXPECT_TRUE(ProcessingErrorsOps::insertError(
                dbm, "/test.jpg", ServerMode::EMBEDDING, -1, "Balanced mode error", "General"));

            EXPECT_TRUE(ProcessingErrorsOps::insertError(
                dbm, "/test.jpg", ServerMode::EMBEDDING, -1, "Quality mode error", "General"));
        }

        TEST(ProcessingErrorsOpsTest, InsertErrorWithEmptySource)
        {
            std::string db_path = testDbPath("test_processing_errors_empty_source.sqlite");
            removeDbAndWal(db_path);

            DatabaseManager dbm(db_path);
            ASSERT_TRUE(dbm.initialize());
            ASSERT_TRUE(ProcessingErrorsOps::ensureTable(dbm));

            // Insert error with empty source (optional parameter)
            bool result = ProcessingErrorsOps::insertError(
                dbm,
                "/path/to/file.jpg",
                ServerMode::EMBEDDING,
                -1,
                "Error without source",
                "" // Empty source
            );

            EXPECT_TRUE(result);
        }

        TEST(ProcessingErrorsOpsTest, InsertErrorVariousErrorCodes)
        {
            std::string db_path = testDbPath("test_processing_errors_codes.sqlite");
            removeDbAndWal(db_path);

            DatabaseManager dbm(db_path);
            ASSERT_TRUE(dbm.initialize());
            ASSERT_TRUE(ProcessingErrorsOps::ensureTable(dbm));

            // Test different error codes
            std::vector<int> error_codes = {-1, -2, -3, -4, -5, -6, -99, -101, -102, -103};

            for (int code : error_codes)
            {
                EXPECT_TRUE(ProcessingErrorsOps::insertError(
                    dbm,
                    "/file_" + std::to_string(code) + ".jpg",
                    ServerMode::EMBEDDING,
                    code,
                    "Error code " + std::to_string(code),
                    "TestSource"));
            }
        }
    }
}
