#include <gtest/gtest.h>
#include "core/media_processing_orchestrator.hpp"
#include "core/database_manager.hpp"
#include <fstream>
#include <filesystem>
#include "core/file_utils.hpp"

namespace fs = std::filesystem;

class MediaProcessingOrchestratorTest : public ::testing::Test
{
protected:
    std::string db_path = "test_orchestrator.db";
    std::string test_dir = "test_orchestrator_files";

    void SetUp() override
    {
        // Remove any existing test DB
        if (fs::exists(db_path))
            fs::remove(db_path);

        // Create test directory and files
        fs::create_directories(test_dir);

        // Create a test image file (just a placeholder)
        std::ofstream(test_dir + "/test.jpg") << "fake jpeg data";

        // Create a test text file
        std::ofstream(test_dir + "/test.txt") << "text file content";
    }

    void TearDown() override
    {
        if (fs::exists(db_path))
            fs::remove(db_path);

        if (fs::exists(test_dir))
            fs::remove_all(test_dir);
    }
};

TEST_F(MediaProcessingOrchestratorTest, EmitsEventsAndUpdatesDB)
{
    DatabaseManager db(db_path);
    // Add a supported and unsupported file with actual paths
    std::string supported = test_dir + "/test.jpg";
    std::string unsupported = test_dir + "/test.txt";
    db.storeScannedFile(supported, FileUtils::computeFileHash(supported));
    db.storeScannedFile(unsupported, FileUtils::computeFileHash(unsupported));
    MediaProcessingOrchestrator orchestrator(db_path);
    std::vector<FileProcessingEvent> events;
    orchestrator.processAllScannedFiles(2).subscribe(
        [&](const FileProcessingEvent &evt)
        { events.push_back(evt); },
        nullptr,
        [&]()
        {
            // Check that both files emitted events
            ASSERT_EQ(events.size(), 2);
            // One should be unsupported
            auto it = std::find_if(events.begin(), events.end(), [](const FileProcessingEvent &e)
                                   { return !e.success; });
            ASSERT_TRUE(it != events.end());
            EXPECT_TRUE(it->file_path == unsupported);
            // Test that files are processed correctly
            auto scanned = db.getAllScannedFiles();
            EXPECT_EQ(scanned.size(), 2); // Both files should be scanned
        });
}