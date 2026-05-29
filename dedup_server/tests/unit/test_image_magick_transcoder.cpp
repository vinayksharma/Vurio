#include <gtest/gtest.h>
#include "media_processors/image/backends/image_magick_transcoder.hpp"
#include "media_processors/image/backends/raw_file_detector.hpp"
#include "test_utils.hpp"
#include <vector>
#include <string>
#include <filesystem>
#include <fstream>

using namespace MediaDedup;

class ImageMagickTranscoderTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Setup any required test environment
    }

    void TearDown() override
    {
        // Clean up
    }
    
    // Create a very basic valid JPEG file by writing minimal valid magic bytes
    // (This might not be enough for ImageMagick to successfully transcode if it's truly empty,
    // but we can try)
    std::string createDummyImage(const std::string& prefix, const std::string& extension) {
        std::string path = MediaDedup::Test::TestUtils::generateTempFilePath(prefix, extension);
        std::string content;
        
        if (extension == "jpg" || extension == "jpeg") {
            content.push_back((char)0xFF); content.push_back((char)0xD8); content.push_back((char)0xFF);
        } else if (extension == "gif") {
            content = "GIF89a";
        } else if (extension == "cr2") {
            // TIFF header
            content.push_back((char)0x49); content.push_back((char)0x49); content.push_back((char)0x2A); content.push_back((char)0x00);
        } else {
            content = "dummy";
        }
        
        // Let's copy a real image if we have one, to ensure ImageMagick doesn't fail on invalid image content.
        // Actually, ImageMagick will fail on a 3-byte JPEG file because it's not a complete valid JPEG.
        // So `transcodeToJpeg` will return false.
        // But we can at least test initialization and basic error handling.
        
        MediaDedup::Test::TestUtils::createTempFile(content, path);
        return path;
    }
};

TEST_F(ImageMagickTranscoderTest, Initialization_Success)
{
    ImageMagickTranscoder transcoder;
    EXPECT_TRUE(transcoder.isValid());
}

TEST_F(ImageMagickTranscoderTest, TranscodeToJpeg_EmptyPath_ReturnsFalse)
{
    ImageMagickTranscoder transcoder;
    std::vector<std::uint8_t> out_data;
    EXPECT_FALSE(transcoder.transcodeToJpeg("", out_data));
}

TEST_F(ImageMagickTranscoderTest, TranscodeToJpeg_NonExistentFile_ReturnsFalse)
{
    ImageMagickTranscoder transcoder;
    std::vector<std::uint8_t> out_data;
    EXPECT_FALSE(transcoder.transcodeToJpeg("/path/does/not/exist.jpg", out_data));
}

TEST_F(ImageMagickTranscoderTest, TranscodeToJpeg_InvalidImageFile_ReturnsFalse)
{
    ImageMagickTranscoder transcoder;
    std::string path = createDummyImage("invalid_image", "jpg");
    std::vector<std::uint8_t> out_data;
    
    // It should fail gracefully since it's not a real image
    EXPECT_FALSE(transcoder.transcodeToJpeg(path, out_data));
    
    MediaDedup::Test::TestUtils::deleteFile(path);
}
