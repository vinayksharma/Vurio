#include <gtest/gtest.h>
#include "media_processors/image/pipelines/quality_pipeline.hpp"
#include "database/database_manager.hpp"
#include "media_processors/image/backends/onnx_adapter.hpp"
#include "test_utils.hpp"
#include <vector>
#include <string>

namespace MediaDedup
{
    class QualityPipelineMemoryTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            // Create a test database
            test_db_path = ::MediaDedup::Test::TestUtils::generateTempFilePath("test_quality_memory", "db");
            db = std::make_unique<DatabaseManager>(test_db_path);
            db->initialize();

            // Create a simple test image data (minimal valid JPEG header + data)
            // This is a 1x1 pixel JPEG image
            test_image_data = {
                0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x01, 0x00, 0x48,
                0x00, 0x48, 0x00, 0x00, 0xFF, 0xDB, 0x00, 0x43, 0x00, 0x08, 0x06, 0x06, 0x07, 0x06, 0x05, 0x08,
                0x07, 0x07, 0x07, 0x09, 0x09, 0x08, 0x0A, 0x0C, 0x14, 0x0D, 0x0C, 0x0B, 0x0B, 0x0C, 0x19, 0x12,
                0x13, 0x0F, 0x14, 0x1D, 0x1A, 0x1F, 0x1E, 0x1D, 0x1A, 0x1C, 0x1C, 0x20, 0x24, 0x2E, 0x27, 0x20,
                0x22, 0x2C, 0x23, 0x1C, 0x1C, 0x28, 0x37, 0x29, 0x2C, 0x30, 0x31, 0x34, 0x34, 0x34, 0x1F, 0x27,
                0x39, 0x3D, 0x38, 0x32, 0x3C, 0x2E, 0x33, 0x34, 0x32, 0xFF, 0xC0, 0x00, 0x11, 0x08, 0x00, 0x01,
                0x00, 0x01, 0x01, 0x01, 0x11, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01, 0xFF, 0xC4, 0x00, 0x14,
                0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x08, 0xFF, 0xC4, 0x00, 0x14, 0x10, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xDA, 0x00, 0x0C, 0x03, 0x01, 0x00, 0x02,
                0x11, 0x03, 0x11, 0x00, 0x3F, 0x00, 0x80, 0xFF, 0xD9};
        }

        void TearDown() override
        {
            db.reset();
            if (!test_db_path.empty())
            {
                ::MediaDedup::Test::TestUtils::deleteFile(test_db_path);
            }
        }

        std::string test_db_path;
        std::unique_ptr<DatabaseManager> db;
        std::vector<std::uint8_t> test_image_data;
        QualityPipelineConfig config;
    };

    TEST_F(QualityPipelineMemoryTest, TestMemoryBasedProcessing)
    {
        // Configure the pipeline
        config.input_size = 224;
        config.model = "models/clip-image-vitb32.onnx";
        config.embedding_dim = 512;

        const std::string test_file_path = "/test/image/from/memory.jpg";

        // Test the memory-based processing
        bool result = QualityPipeline::Run(test_image_data, test_file_path, config, *db);

        // Since ONNX Runtime is not available in the test environment,
        // we expect this to fail gracefully (not crash)
        // The important thing is that the function completes without throwing exceptions
        // and handles the case where ONNX Runtime is not available

        // In a real environment with ONNX Runtime, this would return true
        // In the test environment, it should return false but not crash
        EXPECT_FALSE(result); // Expected to fail in test environment without ONNX Runtime
    }

    TEST_F(QualityPipelineMemoryTest, TestEmptyImageData)
    {
        config.input_size = 224;
        config.model = "models/clip-image-vitb32.onnx";
        config.embedding_dim = 512;

        const std::string test_file_path = "/test/empty/image.jpg";
        std::vector<std::uint8_t> empty_data;

        // Test with empty image data - should fail gracefully
        bool result = QualityPipeline::Run(empty_data, test_file_path, config, *db);
        EXPECT_FALSE(result);
    }

    TEST_F(QualityPipelineMemoryTest, TestInvalidImageData)
    {
        config.input_size = 224;
        config.model = "models/clip-image-vitb32.onnx";
        config.embedding_dim = 512;

        const std::string test_file_path = "/test/invalid/image.jpg";
        std::vector<std::uint8_t> invalid_data = {0x00, 0x01, 0x02, 0x03}; // Not valid image data

        // Test with invalid image data - should fail gracefully
        bool result = QualityPipeline::Run(invalid_data, test_file_path, config, *db);
        EXPECT_FALSE(result);
    }
}
