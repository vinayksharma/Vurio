#include <gtest/gtest.h>
#include "media_processors/image/backends/raw_file_detector.hpp"
#include "test_utils.hpp"
#include <vector>
#include <string>

using namespace MediaDedup;

class RawFileDetectorTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // No setup needed for this test
    }

    void TearDown() override
    {
        // No cleanup needed for this test
    }
};

TEST_F(RawFileDetectorTest, IsRawFile_WithRawExtensions_ReturnsTrue)
{
    // Test various raw file extensions
    std::vector<std::string> raw_files = {
        "test.arw", "test.CR2", "test.dng", "test.nef", "test.orf", "test.pef",
        "test.raf", "test.rw2", "test.srw", "test.3fr", "test.bay", "test.dcr",
        "test.fff", "test.iiq", "test.kdc", "test.mef", "test.mos", "test.mrw",
        "test.nrw", "test.raw", "test.rwl", "test.rwz"};

    for (const auto &file : raw_files)
    {
        EXPECT_TRUE(RawFileDetector::IsRawFile(file)) << "Failed for file: " << file;
    }
}

TEST_F(RawFileDetectorTest, IsRawFile_WithNonRawExtensions_ReturnsFalse)
{
    // Test various non-raw file extensions
    std::vector<std::string> non_raw_files = {
        "test.jpg", "test.jpeg", "test.png", "test.tiff", "test.tif", "test.webp",
        "test.bmp", "test.gif", "test.avi", "test.mp4", "test.mov", "test.mp3",
        "test.wav", "test.txt", "test.pdf", "test.doc"};

    for (const auto &file : non_raw_files)
    {
        EXPECT_FALSE(RawFileDetector::IsRawFile(file)) << "Failed for file: " << file;
    }
}

TEST_F(RawFileDetectorTest, IsRawFile_WithEmptyPath_ReturnsFalse)
{
    EXPECT_FALSE(RawFileDetector::IsRawFile(""));
    EXPECT_FALSE(RawFileDetector::IsRawFile("   "));
}

TEST_F(RawFileDetectorTest, IsRawFile_WithNoExtension_ReturnsFalse)
{
    EXPECT_FALSE(RawFileDetector::IsRawFile("filename"));
    EXPECT_FALSE(RawFileDetector::IsRawFile("/path/to/filename"));
}

TEST_F(RawFileDetectorTest, GetFileExtension_WithVariousPaths_ReturnsCorrectExtension)
{
    EXPECT_EQ(RawFileDetector::GetFileExtension("test.arw"), "arw");
    EXPECT_EQ(RawFileDetector::GetFileExtension("test.CR2"), "cr2");
    EXPECT_EQ(RawFileDetector::GetFileExtension("path/to/file.dng"), "dng");
    EXPECT_EQ(RawFileDetector::GetFileExtension("file.JPG"), "jpg");
    EXPECT_EQ(RawFileDetector::GetFileExtension("file"), "");
    EXPECT_EQ(RawFileDetector::GetFileExtension(""), "");
}

TEST_F(RawFileDetectorTest, GetSupportedRawExtensions_ReturnsExpectedExtensions)
{
    std::vector<std::string> extensions = RawFileDetector::GetSupportedRawExtensions();

    // Check that we have the expected number of extensions
    EXPECT_GE(extensions.size(), 20);

    // Check for some key raw formats
    EXPECT_TRUE(std::find(extensions.begin(), extensions.end(), "arw") != extensions.end());
    EXPECT_TRUE(std::find(extensions.begin(), extensions.end(), "cr2") != extensions.end());
    EXPECT_TRUE(std::find(extensions.begin(), extensions.end(), "dng") != extensions.end());
    EXPECT_TRUE(std::find(extensions.begin(), extensions.end(), "nef") != extensions.end());

    // Check that all extensions are lowercase (letters only, numbers are allowed)
    for (const auto &ext : extensions)
    {
        for (char c : ext)
        {
            if (std::isalpha(c))
            {
                EXPECT_TRUE(std::islower(c)) << "Extension should be lowercase: " << ext;
            }
        }
    }
}

TEST_F(RawFileDetectorTest, IsRawFile_CaseInsensitive_Works)
{
    EXPECT_TRUE(RawFileDetector::IsRawFile("test.ARW"));
    EXPECT_TRUE(RawFileDetector::IsRawFile("test.Cr2"));
    EXPECT_TRUE(RawFileDetector::IsRawFile("test.DNG"));
    EXPECT_TRUE(RawFileDetector::IsRawFile("test.NeF"));
}
