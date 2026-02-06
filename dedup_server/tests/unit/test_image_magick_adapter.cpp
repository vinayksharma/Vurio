#include <gtest/gtest.h>
#include "media_processors/image/backends/image_magick_adapter.hpp"
#include "test_utils.hpp"
#include <vector>
#include <string>
#include <filesystem>
#include <thread>
#include <mutex>
#include <unistd.h>

using namespace MediaDedup;
using namespace MediaDedup::Test;

class ImageMagickAdapterTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Test files from the testset directory
        std::filesystem::path test_data_path = TestUtils::getTestDataPath();
        std::filesystem::path test_set_path = test_data_path / "pictures" / "testset";
        test_files_dir_ = test_set_path.string() + "/";

        // Debug: Check if the test file exists
        std::string test_file = test_files_dir_ + "test.jpg";
        if (!std::filesystem::exists(test_file))
        {
            std::cout << "WARNING: Test file does not exist: " << test_file << std::endl;
        }
        else
        {
            std::cout << "Test file exists: " << test_file << std::endl;
        }

        // Debug: Check current working directory
        char cwd[1024];
        if (getcwd(cwd, sizeof(cwd)) != nullptr)
        {
            std::cout << "Current working directory: " << cwd << std::endl;
        }

        // Raw image files to test
        raw_files_ = {
            "test.arw",
            "test.cr2",
            "test.dng"};

        // Non-raw files for error testing
        non_raw_files_ = {
            "test.jpg",
            "test.png",
            "test.tif"};
    }

    std::string test_files_dir_;
    std::vector<std::string> raw_files_;
    std::vector<std::string> non_raw_files_;
};

TEST_F(ImageMagickAdapterTest, TranscodeToJpeg_WithValidRawFiles_ReturnsTrue)
{
    for (const auto &filename : raw_files_)
    {
        std::string file_path = test_files_dir_ + filename;

        // Skip if file doesn't exist
        if (!std::filesystem::exists(file_path))
        {
            GTEST_SKIP() << "Test file not found: " << file_path;
            continue;
        }

        std::vector<std::uint8_t> jpeg_data;
        bool result = ImageMagickAdapter::TranscodeToJpeg(file_path, jpeg_data);

        EXPECT_TRUE(result) << "Failed to transcode: " << filename;
        EXPECT_FALSE(jpeg_data.empty()) << "JPEG data should not be empty for: " << filename;

        // Verify it's actually JPEG data (starts with JPEG header)
        if (!jpeg_data.empty())
        {
            EXPECT_TRUE(jpeg_data.size() > 2) << "JPEG data too small for: " << filename;
            // JPEG files start with 0xFF 0xD8 (SOI marker)
            bool is_jpeg = (jpeg_data[0] == 0xFF && jpeg_data[1] == 0xD8);
            EXPECT_TRUE(is_jpeg) << "Data doesn't appear to be JPEG format for: " << filename;
        }
    }
}

TEST_F(ImageMagickAdapterTest, TranscodeToJpeg_WithNonRawFiles_ReturnsTrue)
{
    for (const auto &filename : non_raw_files_)
    {
        std::string file_path = test_files_dir_ + filename;

        // Skip if file doesn't exist
        if (!std::filesystem::exists(file_path))
        {
            GTEST_SKIP() << "Test file not found: " << file_path;
            continue;
        }

        std::vector<std::uint8_t> tiff_data;
        bool result = ImageMagickAdapter::TranscodeToJpeg(file_path, tiff_data);

        EXPECT_TRUE(result) << "Failed to transcode: " << filename;
        EXPECT_FALSE(tiff_data.empty()) << "TIFF data should not be empty for: " << filename;
    }
}

TEST_F(ImageMagickAdapterTest, TranscodeToJpeg_WithNonExistentFile_ReturnsFalse)
{
    std::string non_existent_file = test_files_dir_ + "non_existent_file.raw";
    std::vector<std::uint8_t> tiff_data;

    bool result = ImageMagickAdapter::TranscodeToJpeg(non_existent_file, tiff_data);

    EXPECT_FALSE(result);
    EXPECT_TRUE(tiff_data.empty());
}

TEST_F(ImageMagickAdapterTest, TranscodeToJpeg_WithEmptyPath_ReturnsFalse)
{
    std::string empty_path = "";
    std::vector<std::uint8_t> tiff_data;

    bool result = ImageMagickAdapter::TranscodeToJpeg(empty_path, tiff_data);

    EXPECT_FALSE(result);
    EXPECT_TRUE(tiff_data.empty());
}

TEST_F(ImageMagickAdapterTest, TranscodeToJpeg_ThreadSafety_ConcurrentCalls)
{
    // Test thread safety with concurrent calls
    std::string file_path = test_files_dir_ + "test.jpg";
    
    // Skip test if file doesn't exist
    if (!std::filesystem::exists(file_path))
    {
        GTEST_SKIP() << "Test file not found: " << file_path;
    }
    
    const int num_threads = 4;
    const int calls_per_thread = 5;
    std::vector<std::thread> threads;
    std::vector<bool> results(num_threads * calls_per_thread, false);
    std::mutex results_mutex; // Protect results vector access

    for (int t = 0; t < num_threads; ++t)
    {
        threads.emplace_back([this, t, calls_per_thread, &results, &file_path, &results_mutex]()
                             {
            for (int i = 0; i < calls_per_thread; ++i)
            {
                std::vector<std::uint8_t> jpeg_data;
                bool result = ImageMagickAdapter::TranscodeToJpeg(file_path, jpeg_data);
                
                int index = t * calls_per_thread + i;
                {
                    std::lock_guard<std::mutex> lock(results_mutex);
                    results[index] = result && !jpeg_data.empty();
                }
            } });
    }

    // Wait for all threads to complete
    for (auto &thread : threads)
    {
        thread.join();
    }

    // Check that all calls succeeded
    for (size_t i = 0; i < results.size(); ++i)
    {
        EXPECT_TRUE(results[i]) << "Thread safety test failed at index " << i;
    }
}

TEST_F(ImageMagickAdapterTest, TranscodeToJpeg_OutputDataIntegrity)
{
    std::string file_path = test_files_dir_ + "test.jpg";

    // Skip if file doesn't exist
    if (!std::filesystem::exists(file_path))
    {
        GTEST_SKIP() << "Test file not found: " << file_path;
    }

    std::vector<std::uint8_t> jpeg_data;
    bool result = ImageMagickAdapter::TranscodeToJpeg(file_path, jpeg_data);

    ASSERT_TRUE(result);
    ASSERT_FALSE(jpeg_data.empty());

    // Verify JPEG header
    EXPECT_TRUE(jpeg_data.size() > 10);

    // Check for JPEG SOI marker (Start of Image: 0xFF 0xD8)
    EXPECT_EQ(jpeg_data[0], 0xFF) << "Invalid JPEG header - missing 0xFF";
    EXPECT_EQ(jpeg_data[1], 0xD8) << "Invalid JPEG header - missing 0xD8 (SOI)";

    // Check for JPEG JFIF or Exif marker (optional but common)
    // JFIF: 0xFF 0xE0, Exif: 0xFF 0xE1, or DQT: 0xFF 0xDB
    bool has_valid_marker = (jpeg_data[2] == 0xFF &&
                             (jpeg_data[3] == 0xE0 || jpeg_data[3] == 0xE1 ||
                              jpeg_data[3] == 0xDB || jpeg_data[3] == 0xC0));
    EXPECT_TRUE(has_valid_marker) << "JPEG should have valid marker after SOI";
}
