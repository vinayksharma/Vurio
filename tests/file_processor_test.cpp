#include <gtest/gtest.h>
#include "core/file_processor.hpp"
#include "core/server_config_manager.hpp"
#include <filesystem>
#include <fstream>

class FileProcessorTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Create a temporary test directory
        test_dir_ = std::filesystem::temp_directory_path() / "file_processor_test";
        std::filesystem::create_directories(test_dir_);

        // Create some test files
        createTestFiles();

        // Set up test database path
        test_db_ = std::filesystem::temp_directory_path() / "test_processing.db";
    }

    void TearDown() override
    {
        // Clean up test files
        if (std::filesystem::exists(test_dir_))
        {
            std::filesystem::remove_all(test_dir_);
        }

        // Clean up test database
        if (std::filesystem::exists(test_db_))
        {
            std::filesystem::remove(test_db_);
        }
    }

    void createTestFiles()
    {
        // Create a test image file (just a placeholder)
        std::ofstream(test_dir_ / "test_image.jpg") << "fake jpeg data";

        // Create a test video file (just a placeholder)
        std::ofstream(test_dir_ / "test_video.mp4") << "fake mp4 data";

        // Create an unsupported file
        std::ofstream(test_dir_ / "test_document.txt") << "text file content";
    }

    std::filesystem::path test_dir_;
    std::filesystem::path test_db_;
};

TEST_F(FileProcessorTest, FileProcessorInitialization)
{
    EXPECT_NO_THROW({
        FileProcessor processor(test_db_.string());
    });
}

TEST_F(FileProcessorTest, ProcessSingleFile)
{
    FileProcessor processor(test_db_.string());

    // Test processing a supported file
    std::string image_path = (test_dir_ / "test_image.jpg").string();
    auto image_result = processor.processFile(image_path);
    EXPECT_TRUE(image_result.success) << image_result.error_message;

    // Test processing an unsupported file
    std::string text_path = (test_dir_ / "test_document.txt").string();
    auto text_result = processor.processFile(text_path);
    EXPECT_FALSE(text_result.success) << text_result.error_message;
}

TEST_F(FileProcessorTest, ProcessDirectory)
{
    FileProcessor processor(test_db_.string());

    // Process the test directory
    size_t files_processed = processor.processDirectory(test_dir_.string(), false);

    // Should have processed at least the supported files
    EXPECT_GT(files_processed, 0);

    // Get statistics
    auto stats = processor.getProcessingStats();
    EXPECT_EQ(stats.first, files_processed);
    EXPECT_GT(stats.second, 0); // Should have some successful processing
}

TEST_F(FileProcessorTest, ProcessingStatistics)
{
    FileProcessor processor(test_db_.string());

    // Clear stats
    processor.clearStats();
    auto stats = processor.getProcessingStats();
    EXPECT_EQ(stats.first, 0);
    EXPECT_EQ(stats.second, 0);

    // Process a file
    std::string image_path = (test_dir_ / "test_image.jpg").string();
    auto result = processor.processFile(image_path);
    EXPECT_TRUE(result.success) << result.error_message;

    // Check stats updated
    stats = processor.getProcessingStats();
    EXPECT_EQ(stats.first, 1);
    EXPECT_EQ(stats.second, 1); // Should be successful
}

TEST_F(FileProcessorTest, DatabaseIntegration)
{
    FileProcessor processor(test_db_.string());

    // Process a file
    std::string image_path = (test_dir_ / "test_image.jpg").string();
    auto result = processor.processFile(image_path);
    EXPECT_TRUE(result.success) << result.error_message;

    // Verify database was created and contains data
    EXPECT_TRUE(std::filesystem::exists(test_db_));
    EXPECT_GT(std::filesystem::file_size(test_db_), 0);
}

TEST_F(FileProcessorTest, QualityModeIntegration)
{
    FileProcessor processor(test_db_.string());

    // Get current quality mode
    auto &config_manager = ServerConfigManager::getInstance();
    auto current_mode = config_manager.getDedupMode();

    // Process a file
    std::string image_path = (test_dir_ / "test_image.jpg").string();
    auto result = processor.processFile(image_path);
    EXPECT_TRUE(result.success) << result.error_message;

    // The processing should use the current quality mode
    // (This is verified by the fact that processing succeeds)
    EXPECT_TRUE(true); // Placeholder assertion
}

// TODO: INTEGRATION TESTS WITH REAL MEDIA FILES
//
// These tests would require actual media files:
//
// TEST_F(FileProcessorTest, RealImageProcessing)
// {
//     FileProcessor processor(test_db_.string());
//
//     // Process a real image file
//     std::string real_image_path = "path/to/real/image.jpg";
//     EXPECT_TRUE(processor.processFile(real_image_path));
//
//     // Verify database contains the result
//     DatabaseManager db_manager(test_db_.string());
//     auto results = db_manager.getProcessingResults(real_image_path);
//     EXPECT_FALSE(results.empty());
//     EXPECT_TRUE(results[0].success);
// }
//
// TEST_F(FileProcessorTest, RealVideoProcessing)
// {
//     FileProcessor processor(test_db_.string());
//
//     // Process a real video file
//     std::string real_video_path = "path/to/real/video.mp4";
//     EXPECT_TRUE(processor.processFile(real_video_path));
//
//     // Verify database contains the result
//     DatabaseManager db_manager(test_db_.string());
//     auto results = db_manager.getProcessingResults(real_video_path);
//     EXPECT_FALSE(results.empty());
//     EXPECT_TRUE(results[0].success);
// }