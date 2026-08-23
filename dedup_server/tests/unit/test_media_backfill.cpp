#include <gtest/gtest.h>
#include <Poco/Logger.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "core/media_backfill.hpp"
#include "database/database_manager.hpp"
#include "database/scanned_files_ops.hpp"
#include "media_processors/image/backends/exif_reader.hpp"
#include "test_utils.hpp"

namespace MediaDedup
{
    namespace Test
    {
        // Regression coverage for the RAW EXIF backfill's SELECTOR. The
        // original MediaBackfill statements were built but never executed —
        // Poco::Data::RecordSet over an unexecuted statement iterates zero
        // rows — so every backfill sweep silently saw an empty library.
        // These tests observe the returned stats, which the bug zeroes.
        class MediaBackfillRawExifTest : public ::testing::Test
        {
        protected:
            void SetUp() override
            {
                db_path_ = (std::filesystem::path(TestUtils::getTestDataPath()) /
                            "databases" / "test_media_backfill_raw_exif.sqlite")
                               .string();
                std::filesystem::create_directories(
                    std::filesystem::path(db_path_).parent_path());
                std::remove(db_path_.c_str());
                db_ = std::make_unique<DatabaseManager>(db_path_);
                ASSERT_TRUE(db_->initialize());
                ASSERT_TRUE(ScannedFilesOps::ensureTable(*db_));
            }

            void TearDown() override
            {
                db_.reset();
                std::remove(db_path_.c_str());
            }

            // A row as the PRE-FIX LibRaw reader wrote it: combined camera
            // only, exif_make NULL (ExifInfo::make empty -> stored NULL).
            void insertOldReaderRow(const std::string &path)
            {
                ScannedFileRow row;
                row.file_path = path;
                row.file_name = std::filesystem::path(path).filename().string();
                row.location_key = "mediaLocation:test";
                row.processed = 2;
                ASSERT_TRUE(ScannedFilesOps::upsert(*db_, row));

                ExifInfo exif;
                exif.width = 6000;
                exif.height = 4000;
                exif.camera = "Fujifilm X-Pro1";
                exif.datetime = "2019:07:04 12:00:00";
                ASSERT_TRUE(ScannedFilesOps::updateExifMetadata(*db_, path, exif));
            }

            std::string db_path_;
            std::unique_ptr<DatabaseManager> db_;
        };

        TEST_F(MediaBackfillRawExifTest, SelectorFindsOldReaderRawRows)
        {
            insertOldReaderRow("/nowhere/vacation_0001.NEF");

            auto stats = MediaBackfill::backfillRawExif(
                *db_, Poco::Logger::get("MediaBackfillTest"));

            EXPECT_EQ(stats.candidates, 1u);
            EXPECT_EQ(stats.offline, 1); // path does not exist on disk
            EXPECT_EQ(stats.updated, 0);
            EXPECT_EQ(stats.unreadable, 0);
        }

        TEST_F(MediaBackfillRawExifTest, SelectorSkipsHealedAndNonRawRows)
        {
            // Healed RAW row: make present -> out of the selector.
            {
                ScannedFileRow row;
                row.file_path = "/nowhere/healed.NEF";
                row.file_name = "healed.NEF";
                row.location_key = "mediaLocation:test";
                row.processed = 2;
                ASSERT_TRUE(ScannedFilesOps::upsert(*db_, row));
                ExifInfo exif;
                exif.width = 100;
                exif.height = 100;
                exif.camera = "Fujifilm X-Pro1";
                exif.make = "Fujifilm";
                exif.model = "X-Pro1";
                ASSERT_TRUE(ScannedFilesOps::updateExifMetadata(*db_, "/nowhere/healed.NEF", exif));
            }
            // Non-RAW row with a NULL make: not a RAW extension.
            {
                ScannedFileRow row;
                row.file_path = "/nowhere/screenshot.png";
                row.file_name = "screenshot.png";
                row.location_key = "mediaLocation:test";
                row.processed = 2;
                ASSERT_TRUE(ScannedFilesOps::upsert(*db_, row));
                ExifInfo exif;
                exif.width = 100;
                exif.height = 100;
                exif.camera = "SomeApp";
                ASSERT_TRUE(ScannedFilesOps::updateExifMetadata(*db_, "/nowhere/screenshot.png", exif));
            }

            auto stats = MediaBackfill::backfillRawExif(
                *db_, Poco::Logger::get("MediaBackfillTest"));

            EXPECT_EQ(stats.candidates, 0u);
        }

        TEST_F(MediaBackfillRawExifTest, UnreadableCandidateIsCountedNotUpdated)
        {
            auto garbage = (std::filesystem::path(TestUtils::getTestDataPath()) /
                            "databases" / "garbage.nef")
                               .string();
            std::ofstream(garbage) << "this is not a raw file";
            insertOldReaderRow(garbage);

            auto stats = MediaBackfill::backfillRawExif(
                *db_, Poco::Logger::get("MediaBackfillTest"));

            EXPECT_EQ(stats.candidates, 1u);
            EXPECT_EQ(stats.unreadable, 1);
            EXPECT_EQ(stats.updated, 0);
            std::remove(garbage.c_str());
        }

        TEST_F(MediaBackfillRawExifTest, ProgressReportsSweepPosition)
        {
            insertOldReaderRow("/nowhere/one.NEF");
            insertOldReaderRow("/nowhere/two.NEF");

            std::vector<std::pair<std::size_t, std::size_t>> ticks;
            auto stats = MediaBackfill::backfillRawExif(
                *db_, Poco::Logger::get("MediaBackfillTest"),
                [&](std::size_t done, std::size_t total) {
                    ticks.emplace_back(done, total);
                });

            EXPECT_EQ(stats.candidates, 2u);
            ASSERT_GE(ticks.size(), 2u);
            EXPECT_EQ(ticks.front(), (std::pair<std::size_t, std::size_t>{0u, 2u}));
            EXPECT_EQ(ticks.back(), (std::pair<std::size_t, std::size_t>{2u, 2u}));
        }
    }
}
