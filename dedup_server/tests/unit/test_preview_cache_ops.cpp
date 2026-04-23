#include <gtest/gtest.h>
#include <filesystem>
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
