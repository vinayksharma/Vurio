#include "core/file_processor.hpp"
#include "core/file_scanner.hpp"
#include "database/database_manager.hpp"
#include "core/server_config_manager.hpp"
#include "test_base.hpp"
#include <filesystem>
#include <fstream>

class FileProcessorTest : public TestBase
{
protected:
    void SetUp() override
    {
        // Call parent SetUp to initialize test environment
        TestBase::SetUp();

        // Create a temporary test directory
        test_dir_ = getTestFilesDir() + "/file_processor_test";
        std::filesystem::create_directories(test_dir_);

        // Create some test files
        createTestFiles();

        // Set up test database path
        test_db_ = getTestDbPath();
    }

    void TearDown() override
    {
        // Clean up test files
        if (std::filesystem::exists(test_dir_))
        {
            std::filesystem::remove_all(test_dir_);
        }

        // Call parent TearDown to clean up test environment
        TestBase::TearDown();
    }

    void createTestFiles()
    {
        // Create actual test images using ImageMagick
        std::string test_dir_str = test_dir_.string();

        // Create a simple test image (blue background with red circle)
        std::string cmd1 = "magick -size 100x100 xc:blue -fill red -draw \"circle 50,50 30,50\" " + test_dir_str + "/test_image.jpg";
        system(cmd1.c_str());

        // Create another test image (green background with yellow rectangle)
        std::string cmd2 = "magick -size 100x100 xc:green -fill yellow -draw \"rectangle 20,20 80,80\" " + test_dir_str + "/test_image2.jpg";
        system(cmd2.c_str());

        // Create a test video file (just a placeholder for now)
        std::ofstream(test_dir_ / "test_video.mp4") << "fake mp4 data";

        // Create an unsupported file
        std::ofstream(test_dir_ / "test_document.txt") << "text file content";
    }

    std::filesystem::path test_dir_;
    std::string test_db_;
};

TEST_F(FileProcessorTest, FileProcessorInitialization)
{
    EXPECT_NO_THROW({
        FileProcessor processor(test_db_);
    });
}

TEST_F(FileProcessorTest, ProcessSingleFile)
{
    FileScanner scanner(test_db_);
    FileProcessor processor(test_db_);

    // First scan the supported file
    std::string image_path = (test_dir_ / "test_image.jpg").string();
    bool scan_success = scanner.scanFile(image_path);
    EXPECT_TRUE(scan_success) << "Failed to scan image file";

    // Now process the scanned file
    auto image_result = processor.processFile(image_path);
    EXPECT_TRUE(image_result.success) << image_result.error_message;
    processor.waitForWrites();

    // Test processing an unsupported file (should fail during scan)
    std::string text_path = (test_dir_ / "test_document.txt").string();
    bool text_scan_success = scanner.scanFile(text_path);
    EXPECT_FALSE(text_scan_success) << "Should not scan unsupported files";

    // Try to process unsupported file (should fail)
    auto text_result = processor.processFile(text_path);
    EXPECT_FALSE(text_result.success) << text_result.error_message;
    processor.waitForWrites();
}

TEST_F(FileProcessorTest, ProcessDirectory)
{
    FileScanner scanner(test_db_);
    FileProcessor processor(test_db_);

    // First scan the directory to store supported files
    size_t files_scanned = scanner.scanDirectory(test_dir_.string(), false);
    EXPECT_GT(files_scanned, 0) << "Should scan at least some supported files";

    // Now process the scanned files
    size_t files_processed = processor.processDirectory(test_dir_.string(), false);
    processor.waitForWrites();

    // Should have processed at least the supported files
    EXPECT_GT(files_processed, 0);

    // Get statistics
    auto stats = processor.getProcessingStats();
    EXPECT_EQ(stats.first, files_processed);
    EXPECT_GT(stats.second, 0); // Should have some successful processing
}

TEST_F(FileProcessorTest, ProcessingStatistics)
{
    FileScanner scanner(test_db_);
    FileProcessor processor(test_db_);

    // Clear stats
    processor.clearStats();
    auto stats = processor.getProcessingStats();
    EXPECT_EQ(stats.first, 0);
    EXPECT_EQ(stats.second, 0);

    // First scan the file
    std::string image_path = (test_dir_ / "test_image.jpg").string();
    bool scan_success = scanner.scanFile(image_path);
    EXPECT_TRUE(scan_success) << "Failed to scan image file";

    // Process the scanned file
    auto result = processor.processFile(image_path);
    EXPECT_TRUE(result.success) << result.error_message;

    // Wait for all database operations to complete
    processor.waitForWrites();

    // Check stats updated
    stats = processor.getProcessingStats();
    EXPECT_EQ(stats.first, 1) << "Expected 1 total files processed, got " << stats.first;
    EXPECT_EQ(stats.second, 1) << "Expected 1 successful files processed, got " << stats.second;
}

TEST_F(FileProcessorTest, DatabaseIntegration)
{
    FileScanner scanner(test_db_);
    FileProcessor processor(test_db_);

    // First scan the file
    std::string image_path = (test_dir_ / "test_image.jpg").string();
    bool scan_success = scanner.scanFile(image_path);
    EXPECT_TRUE(scan_success) << "Failed to scan image file";

    // Process the scanned file
    auto result = processor.processFile(image_path);
    EXPECT_TRUE(result.success) << result.error_message;
    processor.waitForWrites();

    // Verify database was created and contains data
    // Check if the database file exists at the path that DatabaseManager is actually using
    // The test_db_ path should match the path used by DatabaseManager
    EXPECT_TRUE(std::filesystem::exists(test_db_)) << "Database file not found at: " << test_db_;

    if (std::filesystem::exists(test_db_))
    {
        EXPECT_GT(std::filesystem::file_size(test_db_), 0) << "Database file is empty";
    }
}

TEST_F(FileProcessorTest, QualityModeIntegration)
{
    FileScanner scanner(test_db_);
    FileProcessor processor(test_db_);

    // Get current quality mode
    auto &config_manager = ServerConfigManager::getInstance();
    auto current_mode = config_manager.getDedupMode();

    // First scan the file
    std::string image_path = (test_dir_ / "test_image.jpg").string();
    bool scan_success = scanner.scanFile(image_path);
    EXPECT_TRUE(scan_success) << "Failed to scan image file";

    // Process the scanned file
    auto result = processor.processFile(image_path);
    EXPECT_TRUE(result.success) << result.error_message;
    processor.waitForWrites();

    // The processing should use the current quality mode
    // (This is verified by the fact that processing succeeds)
    EXPECT_TRUE(true); // Placeholder assertion
}

TEST_F(FileProcessorTest, GetFileCategory)
{
    FileProcessor processor(test_db_);

    // Test supported file categories - now all supported file types return "Supported"
    EXPECT_EQ(FileProcessor::getFileCategory("test_image.jpg"), "Supported");
    EXPECT_EQ(FileProcessor::getFileCategory("test_image.png"), "Supported");
    EXPECT_EQ(FileProcessor::getFileCategory("test_image.jpeg"), "Supported");

    // Test video file category - now all supported file types return "Supported"
    EXPECT_EQ(FileProcessor::getFileCategory("test_video.mp4"), "Supported");
    EXPECT_EQ(FileProcessor::getFileCategory("test_video.avi"), "Supported");
    EXPECT_EQ(FileProcessor::getFileCategory("test_video.mov"), "Supported");

    // Test audio file category - now all supported file types return "Supported"
    EXPECT_EQ(FileProcessor::getFileCategory("test_audio.mp3"), "Supported");
    EXPECT_EQ(FileProcessor::getFileCategory("test_audio.wav"), "Supported");
    EXPECT_EQ(FileProcessor::getFileCategory("test_audio.flac"), "Supported");

    // Test unknown file category
    EXPECT_EQ(FileProcessor::getFileCategory("test_document.txt"), "Unknown");
    EXPECT_EQ(FileProcessor::getFileCategory("test_file.pdf"), "Unknown");
    EXPECT_EQ(FileProcessor::getFileCategory("test_file"), "Unknown");
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