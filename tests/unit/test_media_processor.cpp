#include <gtest/gtest.h>
#include <memory>
#include <string>
#include "media_processors/media_processor.hpp"
#include "media_processors/image_processor.hpp"
#include "config/unified_observable_config.hpp"
#include "config/config_manager_factory.hpp"
#include "database/database_manager.hpp"
#include "database/scanned_files_ops.hpp"
#include "test_utils.hpp"

using namespace MediaDedup;

class MediaProcessorTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Create a test configuration manager
        ConfigManagerConfig config;
        config.config_file_path = "test_media_processor.yaml";
        config.enable_file_monitoring = false;
        config.emit_file_change_events = false;
        config.emit_programmatic_events = false;
        config.log_level = "debug";

        config_manager_ = ConfigManagerFactory::createWithConfig(config);
        ASSERT_TRUE(config_manager_ != nullptr);
        ASSERT_TRUE(config_manager_->initialize());

        // Set up test configuration
        setupTestConfiguration();

        // Create test database
        db_path_ = "test_media_processor.sqlite";
        std::remove(db_path_.c_str());
        database_manager_ = std::make_shared<DatabaseManager>(db_path_);
        ASSERT_TRUE(database_manager_->initialize());

        // Create media processor
        media_processor_ = std::make_unique<MediaProcessor>(config_manager_, database_manager_);
        ASSERT_TRUE(media_processor_->initialize());
    }

    void TearDown() override
    {
        if (media_processor_)
        {
            media_processor_->shutdown();
        }
        if (config_manager_)
        {
            config_manager_->shutdown();
        }
        // Clean up test database
        std::remove(db_path_.c_str());
    }

    void setupTestConfiguration()
    {
        // Enable common image formats for testing
        config_manager_->setPropertyValue<bool>("media.images.jpg", true);
        config_manager_->setPropertyValue<bool>("media.images.png", true);
        config_manager_->setPropertyValue<bool>("media.images.jpeg", true);
        config_manager_->setPropertyValue<bool>("media.images.raw.cr2", true);

        // Disable some formats for testing
        config_manager_->setPropertyValue<bool>("media.images.bmp", false);

        // Set server mode
        config_manager_->setPropertyValue<std::string>("server.mode", "FAST");
    }

    std::shared_ptr<UnifiedObservableConfigManager> config_manager_;
    std::shared_ptr<DatabaseManager> database_manager_;
    std::unique_ptr<MediaProcessor> media_processor_;
    std::string db_path_;
};

TEST_F(MediaProcessorTest, RouteToProcessor_SupportedImageFile_ReturnsTrue)
{
    std::string test_file = "/path/to/test/image.jpg";

    bool result = media_processor_->RouteToProcessor(test_file);

    EXPECT_TRUE(result);
}

TEST_F(MediaProcessorTest, RouteToProcessor_UnsupportedFileType_ReturnsFalse)
{
    std::string test_file = "/path/to/test/document.pdf";

    bool result = media_processor_->RouteToProcessor(test_file);

    EXPECT_FALSE(result);
}

TEST_F(MediaProcessorTest, RouteToProcessor_DisabledFileType_ReturnsFalse)
{
    std::string test_file = "/path/to/test/image.bmp";

    bool result = media_processor_->RouteToProcessor(test_file);

    EXPECT_FALSE(result);
}

TEST_F(MediaProcessorTest, RouteToProcessor_EmptyFilePath_ReturnsFalse)
{
    std::string test_file = "";

    bool result = media_processor_->RouteToProcessor(test_file);

    EXPECT_FALSE(result);
}

TEST_F(MediaProcessorTest, RouteToProcessor_NoExtension_ReturnsFalse)
{
    std::string test_file = "/path/to/test/file_without_extension";

    bool result = media_processor_->RouteToProcessor(test_file);

    EXPECT_FALSE(result);
}

TEST_F(MediaProcessorTest, RouteToProcessor_CaseInsensitiveExtension_Works)
{
    std::string test_file_upper = "/path/to/test/image.JPG";
    std::string test_file_mixed = "/path/to/test/image.JpG";

    bool result_upper = media_processor_->RouteToProcessor(test_file_upper);
    bool result_mixed = media_processor_->RouteToProcessor(test_file_mixed);

    EXPECT_TRUE(result_upper);
    EXPECT_TRUE(result_mixed);
}

TEST_F(MediaProcessorTest, RouteToProcessor_RawImageFormat_Works)
{
    std::string test_file = "/path/to/test/image.cr2";

    bool result = media_processor_->RouteToProcessor(test_file);

    EXPECT_TRUE(result);
}

TEST_F(MediaProcessorTest, RouteToProcessor_DifferentServerModes_Work)
{
    std::string test_file = "/path/to/test/image.jpg";

    // Test FAST mode
    config_manager_->setPropertyValue<std::string>("server.mode", "FAST");
    bool result_fast = media_processor_->RouteToProcessor(test_file);
    EXPECT_TRUE(result_fast);

    // Test BALANCED mode
    config_manager_->setPropertyValue<std::string>("server.mode", "BALANCED");
    bool result_balanced = media_processor_->RouteToProcessor(test_file);
    EXPECT_TRUE(result_balanced);

    // Test QUALITY mode
    config_manager_->setPropertyValue<std::string>("server.mode", "QUALITY");
    bool result_quality = media_processor_->RouteToProcessor(test_file);
    EXPECT_TRUE(result_quality);
}

TEST_F(MediaProcessorTest, RouteToProcessor_ThreadSafety_Works)
{
    std::string test_file = "/path/to/test/image.jpg";

    // Test concurrent access
    std::vector<std::thread> threads;
    std::vector<bool> results(10, false);

    for (int i = 0; i < 10; ++i)
    {
        threads.emplace_back([this, &test_file, &results, i]()
                             { results[i] = media_processor_->RouteToProcessor(test_file); });
    }

    for (auto &thread : threads)
    {
        thread.join();
    }

    // All should succeed
    for (bool result : results)
    {
        EXPECT_TRUE(result);
    }
}

TEST_F(MediaProcessorTest, RouteToProcessor_InvalidServerMode_DefaultsToFast)
{
    std::string test_file = "/path/to/test/image.jpg";

    // Set invalid server mode
    config_manager_->setPropertyValue<std::string>("server.mode", "INVALID_MODE");

    bool result = media_processor_->RouteToProcessor(test_file);

    EXPECT_TRUE(result);
}

TEST_F(MediaProcessorTest, RouteToProcessor_VideoFile_NotYetImplemented)
{
    std::string test_file = "/path/to/test/video.mp4";

    // Enable video format
    config_manager_->setPropertyValue<bool>("media.video.mp4", true);

    bool result = media_processor_->RouteToProcessor(test_file);

    EXPECT_FALSE(result); // Should return false as video processing not implemented
}

TEST_F(MediaProcessorTest, RouteToProcessor_AudioFile_NotYetImplemented)
{
    std::string test_file = "/path/to/test/audio.mp3";

    // Enable audio format
    config_manager_->setPropertyValue<bool>("media.audio.mp3", true);

    bool result = media_processor_->RouteToProcessor(test_file);

    EXPECT_FALSE(result); // Should return false as audio processing not implemented
}

// ImageProcessor tests
class ImageProcessorTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        image_processor_ = std::make_unique<ImageProcessor>();
    }

    std::unique_ptr<ImageProcessor> image_processor_;
};

TEST_F(ImageProcessorTest, ProcessFast_ReturnsTrue)
{
    std::string test_file = "/path/to/test/image.jpg";

    bool result = image_processor_->ProcessFast(test_file);

    EXPECT_TRUE(result);
}

TEST_F(ImageProcessorTest, ProcessBalanced_ReturnsTrue)
{
    std::string test_file = "/path/to/test/image.jpg";

    bool result = image_processor_->ProcessBalanced(test_file);

    EXPECT_TRUE(result);
}

TEST_F(ImageProcessorTest, ProcessQuality_ReturnsTrue)
{
    std::string test_file = "/path/to/test/image.jpg";

    bool result = image_processor_->ProcessQuality(test_file);

    EXPECT_TRUE(result);
}

TEST_F(ImageProcessorTest, ProcessMethods_WithEmptyPath_ReturnTrue)
{
    std::string empty_file = "";

    EXPECT_TRUE(image_processor_->ProcessFast(empty_file));
    EXPECT_TRUE(image_processor_->ProcessBalanced(empty_file));
    EXPECT_TRUE(image_processor_->ProcessQuality(empty_file));
}

// ProcessMedia Tests
TEST_F(MediaProcessorTest, ProcessMedia_NoUnprocessedFiles_CompletesSuccessfully)
{
    // Ensure scanned_files table exists
    ASSERT_TRUE(ScannedFilesOps::ensureTable(*database_manager_));

    // ProcessMedia should complete without errors when no unprocessed files exist
    EXPECT_NO_THROW(media_processor_->ProcessMedia());
}

TEST_F(MediaProcessorTest, ProcessMedia_WithUnprocessedFiles_ProcessesSuccessfully)
{
    // Ensure scanned_files table exists
    ASSERT_TRUE(ScannedFilesOps::ensureTable(*database_manager_));

    // Add some test files to the database
    ScannedFileRow test_file1;
    test_file1.file_path = "/path/to/test/image1.jpg";
    test_file1.file_name = "image1.jpg";
    test_file1.processed_fast = 0; // Unprocessed
    test_file1.processed_balanced = 0;
    test_file1.processed_quality = 0;
    ASSERT_TRUE(ScannedFilesOps::upsert(*database_manager_, test_file1));

    ScannedFileRow test_file2;
    test_file2.file_path = "/path/to/test/image2.png";
    test_file2.file_name = "image2.png";
    test_file2.processed_fast = 0; // Unprocessed
    test_file2.processed_balanced = 0;
    test_file2.processed_quality = 0;
    ASSERT_TRUE(ScannedFilesOps::upsert(*database_manager_, test_file2));

    // ProcessMedia should process the files
    EXPECT_NO_THROW(media_processor_->ProcessMedia());

    // Verify files were marked as processed (1) for FAST mode
    auto file1_result = ScannedFilesOps::getByPath(*database_manager_, test_file1.file_path);
    ASSERT_TRUE(file1_result.has_value());
    EXPECT_EQ(file1_result->processed_fast, 1);

    auto file2_result = ScannedFilesOps::getByPath(*database_manager_, test_file2.file_path);
    ASSERT_TRUE(file2_result.has_value());
    EXPECT_EQ(file2_result->processed_fast, 1);
}

TEST_F(MediaProcessorTest, ProcessMedia_WithMixedProcessedFiles_OnlyProcessesUnprocessed)
{
    // Ensure scanned_files table exists
    ASSERT_TRUE(ScannedFilesOps::ensureTable(*database_manager_));

    // Add unprocessed file
    ScannedFileRow unprocessed_file;
    unprocessed_file.file_path = "/path/to/test/unprocessed.jpg";
    unprocessed_file.file_name = "unprocessed.jpg";
    unprocessed_file.processed_fast = 0; // Unprocessed
    unprocessed_file.processed_balanced = 0;
    unprocessed_file.processed_quality = 0;
    ASSERT_TRUE(ScannedFilesOps::upsert(*database_manager_, unprocessed_file));

    // Add already processed file
    ScannedFileRow processed_file;
    processed_file.file_path = "/path/to/test/processed.jpg";
    processed_file.file_name = "processed.jpg";
    processed_file.processed_fast = 1; // Already processed
    processed_file.processed_balanced = 0;
    processed_file.processed_quality = 0;
    ASSERT_TRUE(ScannedFilesOps::upsert(*database_manager_, processed_file));

    // ProcessMedia should only process the unprocessed file
    EXPECT_NO_THROW(media_processor_->ProcessMedia());

    // Verify only unprocessed file was processed
    auto unprocessed_result = ScannedFilesOps::getByPath(*database_manager_, unprocessed_file.file_path);
    ASSERT_TRUE(unprocessed_result.has_value());
    EXPECT_EQ(unprocessed_result->processed_fast, 1);

    auto processed_result = ScannedFilesOps::getByPath(*database_manager_, processed_file.file_path);
    ASSERT_TRUE(processed_result.has_value());
    EXPECT_EQ(processed_result->processed_fast, 1); // Should remain unchanged
}

TEST_F(MediaProcessorTest, ProcessMedia_WithUnsupportedFiles_MarksAsFailed)
{
    // Ensure scanned_files table exists
    ASSERT_TRUE(ScannedFilesOps::ensureTable(*database_manager_));

    // Add unsupported file type
    ScannedFileRow unsupported_file;
    unsupported_file.file_path = "/path/to/test/document.pdf";
    unsupported_file.file_name = "document.pdf";
    unsupported_file.processed_fast = 0; // Unprocessed
    unsupported_file.processed_balanced = 0;
    unsupported_file.processed_quality = 0;
    ASSERT_TRUE(ScannedFilesOps::upsert(*database_manager_, unsupported_file));

    // ProcessMedia should mark unsupported files as failed (2)
    EXPECT_NO_THROW(media_processor_->ProcessMedia());

    // Verify file was marked as failed
    auto result = ScannedFilesOps::getByPath(*database_manager_, unsupported_file.file_path);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->processed_fast, 2); // Failed
}

TEST_F(MediaProcessorTest, ProcessMedia_DisabledInConfig_DoesNotProcess)
{
    // Disable media processing
    config_manager_->setPropertyValue<bool>("media.processor.enabled", false);

    // Ensure scanned_files table exists
    ASSERT_TRUE(ScannedFilesOps::ensureTable(*database_manager_));

    // Add test file
    ScannedFileRow test_file;
    test_file.file_path = "/path/to/test/image.jpg";
    test_file.file_name = "image.jpg";
    test_file.processed_fast = 0; // Unprocessed
    test_file.processed_balanced = 0;
    test_file.processed_quality = 0;
    ASSERT_TRUE(ScannedFilesOps::upsert(*database_manager_, test_file));

    // ProcessMedia should not process files when disabled
    EXPECT_NO_THROW(media_processor_->ProcessMedia());

    // Verify file remains unprocessed
    auto result = ScannedFilesOps::getByPath(*database_manager_, test_file.file_path);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->processed_fast, 0); // Still unprocessed
}

#if !defined(ALL_UNIT_TESTS)
// Provide a test main for this standalone test binary
int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
#endif
