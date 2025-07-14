#include <gtest/gtest.h>
#include "core/media_processor.hpp"
#include "core/dedup_modes.hpp"

class MediaProcessorTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Set up any common test data
    }
};

TEST_F(MediaProcessorTest, SupportedFileExtensions)
{
    auto extensions = MediaProcessor::getSupportedExtensions();

    // Check that we have both image and video extensions
    EXPECT_FALSE(extensions.empty());

    // Check for common image formats
    EXPECT_NE(std::find(extensions.begin(), extensions.end(), "jpg"), extensions.end());
    EXPECT_NE(std::find(extensions.begin(), extensions.end(), "png"), extensions.end());
    EXPECT_NE(std::find(extensions.begin(), extensions.end(), "mp4"), extensions.end());
}

TEST_F(MediaProcessorTest, FileTypeDetection)
{
    // Test image file detection
    EXPECT_TRUE(MediaProcessor::isImageFile("test.jpg"));
    EXPECT_TRUE(MediaProcessor::isImageFile("test.PNG"));
    EXPECT_TRUE(MediaProcessor::isImageFile("test.jpeg"));
    EXPECT_FALSE(MediaProcessor::isImageFile("test.txt"));

    // Test video file detection
    EXPECT_TRUE(MediaProcessor::isVideoFile("test.mp4"));
    EXPECT_TRUE(MediaProcessor::isVideoFile("test.AVI"));
    EXPECT_TRUE(MediaProcessor::isVideoFile("test.mov"));
    EXPECT_FALSE(MediaProcessor::isVideoFile("test.jpg"));
}

TEST_F(MediaProcessorTest, SupportedFileCheck)
{
    // Test supported files
    EXPECT_TRUE(MediaProcessor::isSupportedFile("test.jpg"));
    EXPECT_TRUE(MediaProcessor::isSupportedFile("test.mp4"));
    EXPECT_TRUE(MediaProcessor::isSupportedFile("test.png"));

    // Test unsupported files
    EXPECT_FALSE(MediaProcessor::isSupportedFile("test.txt"));
    EXPECT_FALSE(MediaProcessor::isSupportedFile("test.pdf"));
    EXPECT_FALSE(MediaProcessor::isSupportedFile("test.doc"));
}

TEST_F(MediaProcessorTest, FileExtensionExtraction)
{
    // Test file extension extraction
    EXPECT_EQ(MediaProcessor::getFileExtension("test.jpg"), "jpg");
    EXPECT_EQ(MediaProcessor::getFileExtension("test.PNG"), "png");
    EXPECT_EQ(MediaProcessor::getFileExtension("test.mp4"), "mp4");
    EXPECT_EQ(MediaProcessor::getFileExtension("test"), "");
    EXPECT_EQ(MediaProcessor::getFileExtension("test."), "");
}

TEST_F(MediaProcessorTest, HashGeneration)
{
    std::vector<uint8_t> test_data = {0x01, 0x02, 0x03, 0x04, 0x05};
    std::string hash = MediaProcessor::generateHash(test_data);

    // SHA-256 hash should be 64 characters long
    EXPECT_EQ(hash.length(), 64);

    // Hash should be hexadecimal
    for (char c : hash)
    {
        EXPECT_TRUE(std::isxdigit(c));
    }
}

TEST_F(MediaProcessorTest, ProcessingResultStructure)
{
    // Test default constructor
    ProcessingResult result1;
    EXPECT_FALSE(result1.success);
    EXPECT_TRUE(result1.error_message.empty());
    EXPECT_TRUE(result1.artifact.data.empty());

    // Test constructor with success flag
    ProcessingResult result2(true);
    EXPECT_TRUE(result2.success);
    EXPECT_TRUE(result2.error_message.empty());

    // Test constructor with error message
    ProcessingResult result3(false, "Test error");
    EXPECT_FALSE(result3.success);
    EXPECT_EQ(result3.error_message, "Test error");
}

TEST_F(MediaProcessorTest, MediaArtifactStructure)
{
    MediaArtifact artifact;

    // Test default values
    EXPECT_TRUE(artifact.data.empty());
    EXPECT_TRUE(artifact.format.empty());
    EXPECT_TRUE(artifact.hash.empty());
    EXPECT_EQ(artifact.confidence, 0.0);
    EXPECT_TRUE(artifact.metadata.empty());

    // Test setting values
    artifact.data = {0x01, 0x02, 0x03};
    artifact.format = "test_format";
    artifact.hash = "test_hash";
    artifact.confidence = 0.95;
    artifact.metadata = "{\"test\":\"value\"}";

    EXPECT_EQ(artifact.data.size(), 3);
    EXPECT_EQ(artifact.format, "test_format");
    EXPECT_EQ(artifact.hash, "test_hash");
    EXPECT_EQ(artifact.confidence, 0.95);
    EXPECT_EQ(artifact.metadata, "{\"test\":\"value\"}");
}

TEST_F(MediaProcessorTest, AudioFileDetection)
{
    // Test audio file detection
    EXPECT_TRUE(MediaProcessor::isAudioFile("test.mp3"));
    EXPECT_TRUE(MediaProcessor::isAudioFile("test.WAV"));
    EXPECT_TRUE(MediaProcessor::isAudioFile("test.flac"));
    EXPECT_TRUE(MediaProcessor::isAudioFile("test.ogg"));
    EXPECT_TRUE(MediaProcessor::isAudioFile("test.aac"));
    EXPECT_TRUE(MediaProcessor::isAudioFile("test.m4a"));
    EXPECT_FALSE(MediaProcessor::isAudioFile("test.jpg"));
    EXPECT_FALSE(MediaProcessor::isAudioFile("test.mp4"));
    EXPECT_FALSE(MediaProcessor::isAudioFile("test.txt"));
}

// TODO: INTEGRATION TESTS
//
// These tests would require actual media files and libraries:
//
// TEST_F(MediaProcessorTest, ImageProcessingFast)
// {
//     // Test with actual image file
//     ProcessingResult result = MediaProcessor::processFile("test_image.jpg", DedupMode::FAST);
//     EXPECT_TRUE(result.success);
//     EXPECT_EQ(result.artifact.format, "dhash");
//     EXPECT_GT(result.artifact.confidence, 0.0);
// }
//
// TEST_F(MediaProcessorTest, VideoProcessingBalanced)
// {
//     // Test with actual video file
//     ProcessingResult result = MediaProcessor::processFile("test_video.mp4", DedupMode::BALANCED);
//     EXPECT_TRUE(result.success);
//     EXPECT_EQ(result.artifact.format, "video_phash");
//     EXPECT_GT(result.artifact.confidence, 0.0);
// }
//
// TEST_F(MediaProcessorTest, QualityModeProcessing)
// {
//     // Test with actual media file
//     ProcessingResult result = MediaProcessor::processFile("test_image.jpg", DedupMode::QUALITY);
//     EXPECT_TRUE(result.success);
//     EXPECT_EQ(result.artifact.format, "cnn_embedding");
//     EXPECT_GT(result.artifact.confidence, 0.9);
// }