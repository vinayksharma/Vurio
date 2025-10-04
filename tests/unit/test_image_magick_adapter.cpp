#include <gtest/gtest.h>
#include "media_processors/image/backends/image_magick_adapter.hpp"
#include "test_utils.hpp"
#include <vector>
#include <string>
#include <filesystem>
#include <thread>
#include <unistd.h>

using namespace MediaDedup;

class ImageMagickAdapterTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Test files from the testset directory
        test_files_dir_ = "/Users/vinaysharma/pictures/testset/";

        // Debug: Check if the test file exists
        std::string test_file = test_files_dir_ + "sample.jpg";
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
            "sample.arw",
            "sample.cr2",
            "sample.dng"};

        // Non-raw files for error testing
        non_raw_files_ = {
            "sample.jpg",
            "sample.png",
            "sample.tif"};
    }

    std::string test_files_dir_;
    std::vector<std::string> raw_files_;
    std::vector<std::string> non_raw_files_;
};

TEST_F(ImageMagickAdapterTest, TranscodeToTiff_WithValidRawFiles_ReturnsTrue)
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

        std::vector<std::uint8_t> tiff_data;
        bool result = ImageMagickAdapter::TranscodeToTiff(file_path, tiff_data);

        EXPECT_TRUE(result) << "Failed to transcode: " << filename;
        EXPECT_FALSE(tiff_data.empty()) << "TIFF data should not be empty for: " << filename;

        // Verify it's actually TIFF data (starts with TIFF header)
        if (!tiff_data.empty())
        {
            EXPECT_TRUE(tiff_data.size() > 8) << "TIFF data too small for: " << filename;
            // TIFF files start with either "II" (little-endian) or "MM" (big-endian)
            bool is_tiff = (tiff_data[0] == 'I' && tiff_data[1] == 'I') ||
                           (tiff_data[0] == 'M' && tiff_data[1] == 'M');
            EXPECT_TRUE(is_tiff) << "Data doesn't appear to be TIFF format for: " << filename;
        }
    }
}

TEST_F(ImageMagickAdapterTest, TranscodeToTiff_WithNonRawFiles_ReturnsTrue)
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
        bool result = ImageMagickAdapter::TranscodeToTiff(file_path, tiff_data);

        EXPECT_TRUE(result) << "Failed to transcode: " << filename;
        EXPECT_FALSE(tiff_data.empty()) << "TIFF data should not be empty for: " << filename;
    }
}

TEST_F(ImageMagickAdapterTest, TranscodeToTiff_WithNonExistentFile_ReturnsFalse)
{
    std::string non_existent_file = test_files_dir_ + "non_existent_file.raw";
    std::vector<std::uint8_t> tiff_data;

    bool result = ImageMagickAdapter::TranscodeToTiff(non_existent_file, tiff_data);

    EXPECT_FALSE(result);
    EXPECT_TRUE(tiff_data.empty());
}

TEST_F(ImageMagickAdapterTest, TranscodeToTiff_WithEmptyPath_ReturnsFalse)
{
    std::string empty_path = "";
    std::vector<std::uint8_t> tiff_data;

    bool result = ImageMagickAdapter::TranscodeToTiff(empty_path, tiff_data);

    EXPECT_FALSE(result);
    EXPECT_TRUE(tiff_data.empty());
}

TEST_F(ImageMagickAdapterTest, TranscodeToTiff_ThreadSafety_ConcurrentCalls)
{
    // Test thread safety with concurrent calls
    const int num_threads = 4;
    const int calls_per_thread = 5;
    std::vector<std::thread> threads;
    std::vector<bool> results(num_threads * calls_per_thread, false);

    for (int t = 0; t < num_threads; ++t)
    {
        threads.emplace_back([this, t, calls_per_thread, &results]()
                             {
            for (int i = 0; i < calls_per_thread; ++i)
            {
                std::string file_path = test_files_dir_ + "sample.jpg";
                
                // Skip if file doesn't exist
                if (!std::filesystem::exists(file_path))
                {
                    continue;
                }
                
                std::vector<std::uint8_t> tiff_data;
                bool result = ImageMagickAdapter::TranscodeToTiff(file_path, tiff_data);
                
                int index = t * calls_per_thread + i;
                results[index] = result && !tiff_data.empty();
            } });
    }

    // Wait for all threads to complete
    for (auto &thread : threads)
    {
        thread.join();
    }

    // Check that all calls succeeded
    for (bool result : results)
    {
        EXPECT_TRUE(result);
    }
}

TEST_F(ImageMagickAdapterTest, TranscodeToTiff_OutputDataIntegrity)
{
    std::string file_path = test_files_dir_ + "sample.jpg";

    // Skip if file doesn't exist
    if (!std::filesystem::exists(file_path))
    {
        GTEST_SKIP() << "Test file not found: " << file_path;
    }

    std::vector<std::uint8_t> tiff_data;
    bool result = ImageMagickAdapter::TranscodeToTiff(file_path, tiff_data);

    ASSERT_TRUE(result);
    ASSERT_FALSE(tiff_data.empty());

    // Verify TIFF header
    EXPECT_TRUE(tiff_data.size() > 8);

    // Check for TIFF magic number
    bool is_little_endian = (tiff_data[0] == 'I' && tiff_data[1] == 'I');
    bool is_big_endian = (tiff_data[0] == 'M' && tiff_data[1] == 'M');
    EXPECT_TRUE(is_little_endian || is_big_endian) << "Invalid TIFF header";

    // Check for TIFF version number (42)
    if (is_little_endian)
    {
        uint16_t version = tiff_data[2] | (tiff_data[3] << 8);
        EXPECT_EQ(version, 42) << "Invalid TIFF version";
    }
    else if (is_big_endian)
    {
        uint16_t version = (tiff_data[2] << 8) | tiff_data[3];
        EXPECT_EQ(version, 42) << "Invalid TIFF version";
    }
}
