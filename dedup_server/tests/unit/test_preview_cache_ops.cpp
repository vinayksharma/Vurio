#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include "database/database_manager.hpp"
#include "database/preview_cache_ops.hpp"
#include "database/scanned_files_ops.hpp"

using namespace MediaDedup;

class PreviewCacheOpsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        db_path_ = (std::filesystem::temp_directory_path() / "mds_preview_cache_test.sqlite").string();
        std::remove(db_path_.c_str());

        db_manager_ = std::make_shared<DatabaseManager>(db_path_);
        ASSERT_TRUE(db_manager_->initialize());

        ASSERT_TRUE(PreviewCacheOps::ensureTable(*db_manager_));
        
        // Required for foreign keys or path updates if tested together
        ASSERT_TRUE(ScannedFilesOps::ensureTable(*db_manager_));
    }

    void TearDown() override
    {
        db_manager_.reset();
        if (std::filesystem::exists(db_path_))
        {
            std::filesystem::remove(db_path_);
        }
    }

    std::string db_path_;
    std::shared_ptr<DatabaseManager> db_manager_;
};

TEST_F(PreviewCacheOpsTest, EnsureTable_CreatesTableSuccessfully)
{
    // ensureTable was called in SetUp, let's just make sure it returns true again safely
    EXPECT_TRUE(PreviewCacheOps::ensureTable(*db_manager_));
}

TEST_F(PreviewCacheOpsTest, UpsertAndGet_Success)
{
    std::string source_path = "/path/to/source.cr2";
    std::string cached_path = "/path/to/cache/preview.jpg";

    PreviewCacheRecord record;
    record.source_path = source_path;
    record.cached_path = cached_path;

    EXPECT_TRUE(PreviewCacheOps::upsertPreview(*db_manager_, record));

    auto result = PreviewCacheOps::getPreview(*db_manager_, source_path);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->source_path, source_path);
    EXPECT_EQ(result->cached_path, cached_path);
}

TEST_F(PreviewCacheOpsTest, Upsert_UpdatesExistingEntry)
{
    std::string source_path = "/path/to/source.cr2";
    std::string first_cached_path = "/path/to/cache/preview1.jpg";
    std::string second_cached_path = "/path/to/cache/preview2.jpg";

    PreviewCacheRecord record;
    record.source_path = source_path;
    record.cached_path = first_cached_path;

    EXPECT_TRUE(PreviewCacheOps::upsertPreview(*db_manager_, record));
    
    auto result1 = PreviewCacheOps::getPreview(*db_manager_, source_path);
    ASSERT_TRUE(result1.has_value());
    EXPECT_EQ(result1->cached_path, first_cached_path);

    record.cached_path = second_cached_path;
    EXPECT_TRUE(PreviewCacheOps::upsertPreview(*db_manager_, record));

    auto result2 = PreviewCacheOps::getPreview(*db_manager_, source_path);
    ASSERT_TRUE(result2.has_value());
    EXPECT_EQ(result2->cached_path, second_cached_path);
}

TEST_F(PreviewCacheOpsTest, DeleteBySourcePath_RemovesEntry)
{
    std::string source_path = "/path/to/source.cr2";
    std::string cached_path = "/path/to/cache/preview.jpg";

    PreviewCacheRecord record;
    record.source_path = source_path;
    record.cached_path = cached_path;

    EXPECT_TRUE(PreviewCacheOps::upsertPreview(*db_manager_, record));
    
    EXPECT_TRUE(PreviewCacheOps::deletePreview(*db_manager_, source_path));

    auto result = PreviewCacheOps::getPreview(*db_manager_, source_path);
    EXPECT_FALSE(result.has_value());
}

// QW-8 (backend_optimization_plan.md): every generation site builds its record
// via makeRecord so the freshness fields are populated. A record with
// source_modified_at = 0 permanently fails ingest's `== source_mtime` check
// and the RAW gets re-transcoded although a good preview exists.
TEST_F(PreviewCacheOpsTest, MakeRecord_PopulatesFreshnessFields)
{
    namespace fs = std::filesystem;
    fs::path source = fs::temp_directory_path() / "qw8_source.arw";
    fs::path cached = fs::temp_directory_path() / "qw8_cached.jpg";
    {
        std::ofstream(source.string()) << "raw-bytes-raw-bytes";
        std::ofstream(cached.string()) << "jpeg";
    }

    PreviewCacheRecord record = PreviewCacheOps::makeRecord(source.string(), cached.string());

    EXPECT_EQ(record.source_path, source.string());
    EXPECT_EQ(record.cached_path, cached.string());
    EXPECT_GT(record.source_modified_at, 0) << "zeroed mtime defeats the freshness check";
    EXPECT_EQ(record.file_size_bytes, static_cast<int64_t>(fs::file_size(cached)));
    EXPECT_GT(record.created_at, 0);
    EXPECT_EQ(record.last_accessed_at, record.created_at);

    // And the mtime matches what ingest computes for the same file.
    auto ftime = fs::last_write_time(source);
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    EXPECT_NEAR(static_cast<double>(record.source_modified_at),
                static_cast<double>(std::chrono::system_clock::to_time_t(sctp)), 1.0);

    fs::remove(source);
    fs::remove(cached);
}

TEST_F(PreviewCacheOpsTest, MakeRecord_MissingSourceYieldsZeroMtime)
{
    namespace fs = std::filesystem;
    fs::path cached = fs::temp_directory_path() / "qw8_cached_only.jpg";
    std::ofstream(cached.string()) << "jpeg";

    PreviewCacheRecord record = PreviewCacheOps::makeRecord("/does/not/exist.arw", cached.string());
    EXPECT_EQ(record.source_modified_at, 0);
    EXPECT_GT(record.file_size_bytes, 0);

    fs::remove(cached);
}

TEST_F(PreviewCacheOpsTest, GetBySourcePath_NotFound_ReturnsNullopt)
{
    auto result = PreviewCacheOps::getPreview(*db_manager_, "/nonexistent/path");
    EXPECT_FALSE(result.has_value());
}

TEST_F(PreviewCacheOpsTest, UpdateFilePathInPreviewCache_UpdatesSourcePath)
{
    std::string old_path = "/old/path/source.cr2";
    std::string new_path = "/new/path/source.cr2";
    std::string cached_path = "/path/to/cache/preview.jpg";

    PreviewCacheRecord record;
    record.source_path = old_path;
    record.cached_path = cached_path;

    // Insert with old path
    EXPECT_TRUE(PreviewCacheOps::upsertPreview(*db_manager_, record));

    // Verify it exists at old path
    auto old_result = PreviewCacheOps::getPreview(*db_manager_, old_path);
    ASSERT_TRUE(old_result.has_value());

    // Call update through ScannedFilesOps
    int updated = ScannedFilesOps::updateFilePathInPreviewCache(*db_manager_, old_path, new_path);
    EXPECT_EQ(updated, 1);

    // Verify it no longer exists at old path
    auto missing_old = PreviewCacheOps::getPreview(*db_manager_, old_path);
    EXPECT_FALSE(missing_old.has_value());

    // Verify it exists at new path with correct cached path
    auto new_result = PreviewCacheOps::getPreview(*db_manager_, new_path);
    ASSERT_TRUE(new_result.has_value());
    EXPECT_EQ(new_result->source_path, new_path);
    EXPECT_EQ(new_result->cached_path, cached_path);
}

#ifdef STANDALONE_MAIN_TEST_PREVIEW_CACHE
int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
#endif
