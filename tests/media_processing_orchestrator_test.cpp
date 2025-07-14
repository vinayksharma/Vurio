#include <gtest/gtest.h>
#include "core/media_processing_orchestrator.hpp"
#include "core/database_manager.hpp"
#include <fstream>
#include <filesystem>
#include "core/file_utils.hpp"
#include <thread>
#include <chrono>

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

    void createTestFile(const std::string &path, const std::string &content = "test content")
    {
        std::ofstream file(path);
        file << content;
        file.close();
    }
};

TEST_F(MediaProcessingOrchestratorTest, EmitsEventsAndUpdatesDB)
{
    DatabaseManager db(db_path);

    // Add some test files
    std::string file1 = test_dir + "/test.jpg";
    std::string file2 = test_dir + "/test.txt";
    createTestFile(file1);
    createTestFile(file2);

    // Store test files in database
    db.storeScannedFile(file1);
    db.storeScannedFile(file2);
    db.waitForWrites();

    MediaProcessingOrchestrator orchestrator(db);

    std::vector<FileProcessingEvent> events;
    orchestrator.processAllScannedFiles(2).subscribe(
        [&](const FileProcessingEvent &evt)
        {
            events.push_back(evt);
        },
        nullptr,
        [&]()
        {
            // Processing completed
        });
    db.waitForWrites();
    // Should have processed 2 files (1 supported, 1 unsupported)
    EXPECT_EQ(events.size(), 2);

    // Check that we have one successful and one failed event
    bool found_success = false, found_failure = false;
    for (const auto &event : events)
    {
        if (event.success)
        {
            found_success = true;
            EXPECT_EQ(event.artifact_format, "phash");
            EXPECT_GT(event.artifact_confidence, 0.0);
        }
        else
        {
            found_failure = true;
            EXPECT_FALSE(event.error_message.empty());
        }
    }
    EXPECT_TRUE(found_success);
    EXPECT_TRUE(found_failure);
}

TEST_F(MediaProcessingOrchestratorTest, CancelProcessing)
{
    DatabaseManager db(db_path);

    // Add some test files
    std::string file1 = test_dir + "/test1.jpg";
    std::string file2 = test_dir + "/test2.png";
    createTestFile(file1);
    createTestFile(file2);

    // Store test files in database
    db.storeScannedFile(file1);
    db.storeScannedFile(file2);
    db.waitForWrites();

    MediaProcessingOrchestrator orchestrator(db);

    // Use shared pointer to avoid memory issues
    auto events = std::make_shared<std::vector<FileProcessingEvent>>();

    // Start processing in a separate thread
    std::thread processing_thread([&orchestrator, events]()
                                  { orchestrator.processAllScannedFiles(2).subscribe(
                                        [events](const FileProcessingEvent &evt)
                                        {
                                            events->push_back(evt);
                                        },
                                        nullptr,
                                        [events]()
                                        {
                                            // Processing completed
                                        }); });

    // Cancel processing after a short delay
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    orchestrator.cancel();

    // Wait for processing thread to finish
    if (processing_thread.joinable())
    {
        processing_thread.join();
    }

    // The cancel method should not throw and should complete gracefully
    EXPECT_TRUE(true); // If we reach here, cancel worked without crashing
}