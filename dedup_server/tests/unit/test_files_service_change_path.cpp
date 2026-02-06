#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>

#include "filesmanager/files_service.hpp"
#include "database/database_manager.hpp"
#include "database/scanned_files_ops.hpp"
#include "database/user_settings_service.hpp"
#include "database/image_artifacts_ops.hpp"
#include "database/processing_errors_ops.hpp"
#include "database/duplicate_groups_ops.hpp"
#include "database/thumbnail_cache_ops.hpp"
#include "config/unified_observable_config.hpp"

using namespace MediaDedup;

class FilesServiceChangePathTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Create temporary database
        db_path_ = (std::filesystem::temp_directory_path() / "mds_change_path_test.db").string();
        std::remove(db_path_.c_str());

        db_ = std::make_shared<DatabaseManager>(db_path_);
        ASSERT_TRUE(db_->initialize());

        // Initialize required tables
        UserSettingsService user_settings(*db_);
        ASSERT_TRUE(user_settings.initialize());
        ASSERT_TRUE(ScannedFilesOps::ensureTable(*db_));
        ASSERT_TRUE(ImageArtifactsOps::ensureTable(*db_));
        ASSERT_TRUE(ProcessingErrorsOps::ensureTable(*db_));
        ASSERT_TRUE(DuplicateGroupsOps::ensureTables(*db_));
        ASSERT_TRUE(ThumbnailCacheOps::ensureTable(*db_));

        // Create config manager
        config_manager_ = std::make_shared<UnifiedObservableConfigManager>("", false);
        ASSERT_TRUE(config_manager_->initialize());

        // Create files service
        files_service_ = std::make_shared<FilesService>(*db_, config_manager_);

        // Create temporary directories for testing
        old_path_ = (std::filesystem::temp_directory_path() / "old_media").string();
        new_path_ = (std::filesystem::temp_directory_path() / "new_media").string();

        std::filesystem::create_directories(old_path_);
        std::filesystem::create_directories(new_path_);
    }

    void TearDown() override
    {
        // Clean up database
        if (std::filesystem::exists(db_path_))
        {
            std::remove(db_path_.c_str());
        }

        // Clean up test directories
        if (std::filesystem::exists(old_path_))
        {
            std::filesystem::remove_all(old_path_);
        }
        if (std::filesystem::exists(new_path_))
        {
            std::filesystem::remove_all(new_path_);
        }
    }

    // Helper to create a test file with specific content and size
    std::string createTestFile(const std::string &base_path, const std::string &relative_path, size_t size_bytes)
    {
        std::filesystem::path full_path = std::filesystem::path(base_path) / relative_path;
        std::filesystem::create_directories(full_path.parent_path());

        std::ofstream file(full_path);
        file << std::string(size_bytes, 'A');
        file.close();

        return full_path.string();
    }

    // Helper to register a location and add files
    void registerLocationAndAddFiles(const std::string &path, const std::vector<std::pair<std::string, size_t>> &files)
    {
        ASSERT_TRUE(files_service_->registerMediaLocation(path));

        for (const auto &[relative_path, size] : files)
        {
            std::string full_path = createTestFile(path, relative_path, size);

            ScannedFileRow row;
            row.file_path = full_path;
            row.relative_path = relative_path;
            row.file_name = std::filesystem::path(relative_path).filename().string();
            row.share_name = "";
            row.file_metadata = nlohmann::json({
                {"sizeBytes", size},
                {"modifiedAtEpochMs", 1000000},
                {"createdAt", 1000000}
            }).dump();
            row.processed = 0;
            row.links = "";
            row.is_network_file = false;
            row.location_key = FilesService::makeMediaLocationKey(path);

            ASSERT_TRUE(ScannedFilesOps::upsert(*db_, row));
        }
    }

    std::string db_path_;
    std::shared_ptr<DatabaseManager> db_;
    std::shared_ptr<UnifiedObservableConfigManager> config_manager_;
    std::shared_ptr<FilesService> files_service_;
    std::string old_path_;
    std::string new_path_;
};

// Test successful path change
TEST_F(FilesServiceChangePathTest, ChangePath_Success)
{
    // Register old location and add files
    std::vector<std::pair<std::string, size_t>> files = {
        {"subdir1/file1.jpg", 1024},
        {"subdir1/file2.jpg", 2048},
        {"subdir2/file3.jpg", 3072},
        {"file4.jpg", 4096}
    };
    registerLocationAndAddFiles(old_path_, files);

    // Copy files to new location
    for (const auto &[relative_path, size] : files)
    {
        createTestFile(new_path_, relative_path, size);
    }

    // Change path
    auto result = files_service_->changeMediaLocationPath(old_path_, new_path_, 4);

    // Verify success
    ASSERT_TRUE(result.success);
    ASSERT_FALSE(result.partial_success);
    ASSERT_EQ(result.files_verified, 4);
    ASSERT_EQ(result.files_verified_success, 4);
    ASSERT_EQ(result.total_files, 4);
    ASSERT_EQ(result.files_updated, 4);
    ASSERT_EQ(result.files_failed, 0);
    ASSERT_GE(result.verification_success_rate, 0.80);

    // Verify files were updated in database
    for (const auto &[relative_path, size] : files)
    {
        std::string expected_new_path = (std::filesystem::path(new_path_) / relative_path).string();
        auto file = ScannedFilesOps::getByPath(*db_, expected_new_path);
        ASSERT_TRUE(file.has_value());
        EXPECT_EQ(file->location_key, FilesService::makeMediaLocationKey(new_path_));
    }
}

// Test verification failure (below 80% threshold)
TEST_F(FilesServiceChangePathTest, ChangePath_VerificationFailure)
{
    // Register old location and add files
    std::vector<std::pair<std::string, size_t>> files = {
        {"file1.jpg", 1024},
        {"file2.jpg", 2048},
        {"file3.jpg", 3072},
        {"file4.jpg", 4096},
        {"file5.jpg", 5120}
    };
    registerLocationAndAddFiles(old_path_, files);

    // Copy only 2 out of 5 files to new location (40% success rate)
    createTestFile(new_path_, "file1.jpg", 1024);
    createTestFile(new_path_, "file2.jpg", 2048);

    // Change path should fail verification
    auto result = files_service_->changeMediaLocationPath(old_path_, new_path_, 5);

    // Verify failure
    ASSERT_FALSE(result.success);
    ASSERT_FALSE(result.partial_success);
    ASSERT_EQ(result.files_verified, 5);
    ASSERT_LT(result.files_verified_success, 5);
    ASSERT_LT(result.verification_success_rate, 0.80);
    ASSERT_FALSE(result.error_message.empty());

    // Verify old paths still exist in database
    for (const auto &[relative_path, size] : files)
    {
        std::string old_full_path = (std::filesystem::path(old_path_) / relative_path).string();
        auto file = ScannedFilesOps::getByPath(*db_, old_full_path);
        ASSERT_TRUE(file.has_value());
    }
}

// Test old path not registered
TEST_F(FilesServiceChangePathTest, ChangePath_OldPathNotRegistered)
{
    auto result = files_service_->changeMediaLocationPath("/nonexistent/old", new_path_, 20);

    ASSERT_FALSE(result.success);
    ASSERT_FALSE(result.partial_success);
    ASSERT_TRUE(result.error_message.find("not registered") != std::string::npos);
}

// Test new path doesn't exist
TEST_F(FilesServiceChangePathTest, ChangePath_NewPathDoesNotExist)
{
    registerLocationAndAddFiles(old_path_, {{"file1.jpg", 1024}});

    auto result = files_service_->changeMediaLocationPath(old_path_, "/nonexistent/new", 20);

    ASSERT_FALSE(result.success);
    ASSERT_FALSE(result.partial_success);
    ASSERT_TRUE(result.error_message.find("does not exist") != std::string::npos);
}

// Test empty paths
TEST_F(FilesServiceChangePathTest, ChangePath_EmptyPaths)
{
    auto result1 = files_service_->changeMediaLocationPath("", new_path_, 20);
    ASSERT_FALSE(result1.success);
    ASSERT_TRUE(result1.error_message.find("cannot be empty") != std::string::npos);

    auto result2 = files_service_->changeMediaLocationPath(old_path_, "", 20);
    ASSERT_FALSE(result2.success);
    ASSERT_TRUE(result2.error_message.find("cannot be empty") != std::string::npos);
}

// Test path change with files in subdirectories
TEST_F(FilesServiceChangePathTest, ChangePath_WithSubdirectories)
{
    // Register old location with nested structure
    std::vector<std::pair<std::string, size_t>> files = {
        {"2024/01/image1.jpg", 1024},
        {"2024/01/image2.jpg", 2048},
        {"2024/02/image3.jpg", 3072},
        {"2023/image4.jpg", 4096}
    };
    registerLocationAndAddFiles(old_path_, files);

    // Copy to new location
    for (const auto &[relative_path, size] : files)
    {
        createTestFile(new_path_, relative_path, size);
    }

    auto result = files_service_->changeMediaLocationPath(old_path_, new_path_, 4);

    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.files_updated, 4);

    // Verify all files updated correctly
    for (const auto &[relative_path, size] : files)
    {
        std::string expected_new_path = (std::filesystem::path(new_path_) / relative_path).string();
        auto file = ScannedFilesOps::getByPath(*db_, expected_new_path);
        ASSERT_TRUE(file.has_value());
        EXPECT_EQ(file->relative_path, relative_path);
    }
}

// Test path change updates other tables
TEST_F(FilesServiceChangePathTest, ChangePath_UpdatesOtherTables)
{
    // Register and add file
    registerLocationAndAddFiles(old_path_, {{"test.jpg", 1024}});
    std::string old_file_path = (std::filesystem::path(old_path_) / "test.jpg").string();
    std::string new_file_path = (std::filesystem::path(new_path_) / "test.jpg").string();

    // Add entries to other tables
    ImageEmbeddingRecord embedding;
    embedding.file_path = old_file_path;
    embedding.model = "test-model";
    embedding.dim = 512;
    embedding.embedding_blob = std::vector<std::uint8_t>(512 * 4, 0);
    ASSERT_TRUE(ImageArtifactsOps::upsertEmbedding(*db_, embedding));

    ProcessingErrorsOps::insertError(*db_, old_file_path, ServerMode::EMBEDDING, -1, "test error", "test");

    // Create duplicate group
    int group_id = DuplicateGroupsOps::createGroup(*db_, "EMBEDDING", 1, old_file_path, 1024, "2024-01-01", 0.95);
    ASSERT_GT(group_id, 0);

    // Copy file to new location
    createTestFile(new_path_, "test.jpg", 1024);

    // Change path
    auto result = files_service_->changeMediaLocationPath(old_path_, new_path_, 1);

    ASSERT_TRUE(result.success);

    // Verify updates in other tables
    // Note: ImageArtifactsOps doesn't have a get method, but we can verify via scanned_files
    // The update should have occurred based on the update_details in the result

    // Verify file was updated
    auto file = ScannedFilesOps::getByPath(*db_, new_file_path);
    ASSERT_TRUE(file.has_value());
    
    // Verify update_details show updates in other tables
    ASSERT_GT(result.update_details["image_artifacts"].first, 0);
    ASSERT_GT(result.update_details["processing_errors"].first, 0);
    ASSERT_GT(result.update_details["duplicate_groups"].first, 0);
}

// Test with large number of files (sampling)
TEST_F(FilesServiceChangePathTest, ChangePath_LargeFileSet)
{
    // Create 50 files
    std::vector<std::pair<std::string, size_t>> files;
    for (int i = 1; i <= 50; ++i)
    {
        files.push_back({"file" + std::to_string(i) + ".jpg", 1024 * i});
    }
    registerLocationAndAddFiles(old_path_, files);

    // Copy all to new location
    for (const auto &[relative_path, size] : files)
    {
        createTestFile(new_path_, relative_path, size);
    }

    // Change path with sample size 20
    auto result = files_service_->changeMediaLocationPath(old_path_, new_path_, 20);

    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.files_verified, 20);  // Should sample 20 files
    ASSERT_EQ(result.total_files, 50);
    ASSERT_EQ(result.files_updated, 50);  // Should update all 50 files
}

// Test file size verification
TEST_F(FilesServiceChangePathTest, ChangePath_FileSizeMismatch)
{
    registerLocationAndAddFiles(old_path_, {{"test.jpg", 1024}});

    // Create file with different size at new location
    createTestFile(new_path_, "test.jpg", 2048);  // Different size

    auto result = files_service_->changeMediaLocationPath(old_path_, new_path_, 1);

    // Should fail verification due to size mismatch
    ASSERT_FALSE(result.success);
    ASSERT_LT(result.verification_success_rate, 0.80);
}

// Test relative path reconstruction
TEST_F(FilesServiceChangePathTest, ChangePath_ReconstructsRelativePath)
{
    // Register file with empty relative_path
    ASSERT_TRUE(files_service_->registerMediaLocation(old_path_));

    std::string old_full_path = (std::filesystem::path(old_path_) / "test.jpg").string();
    createTestFile(old_path_, "test.jpg", 1024);

    ScannedFileRow row;
    row.file_path = old_full_path;
    row.relative_path = "";  // Empty relative path
    row.file_name = "test.jpg";
    row.share_name = "";
    row.file_metadata = nlohmann::json({
        {"sizeBytes", 1024},
        {"modifiedAtEpochMs", 1000000}
    }).dump();
    row.processed = 0;
    row.links = "";
    row.is_network_file = false;
    row.location_key = FilesService::makeMediaLocationKey(old_path_);

    ASSERT_TRUE(ScannedFilesOps::upsert(*db_, row));

    // Copy to new location
    createTestFile(new_path_, "test.jpg", 1024);

    auto result = files_service_->changeMediaLocationPath(old_path_, new_path_, 1);

    ASSERT_TRUE(result.success);

    // Verify relative_path was reconstructed
    std::string new_full_path = (std::filesystem::path(new_path_) / "test.jpg").string();
    auto file = ScannedFilesOps::getByPath(*db_, new_full_path);
    ASSERT_TRUE(file.has_value());
    EXPECT_FALSE(file->relative_path.empty());
}

#ifdef STANDALONE_MAIN_CHANGE_PATH
int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
#endif

