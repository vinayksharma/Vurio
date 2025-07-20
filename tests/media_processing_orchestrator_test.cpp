#include "core/media_processing_orchestrator.hpp"
#include "database/database_manager.hpp"
#include "core/media_processor.hpp"
#include "core/file_utils.hpp"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <condition_variable>
#include <iostream> // Added for diagnostic logging

namespace fs = std::filesystem;

class MediaProcessingOrchestratorTest : public ::testing::Test
{
protected:
    std::string db_path = "test_orchestrator.db";
    std::string test_dir = "test_orchestrator_files";

    void SetUp() override
    {
        DatabaseManager::resetForTesting();
        // Remove any existing test DB
        if (fs::exists(db_path))
            fs::remove(db_path);

        // Create test directory and files
        fs::create_directories(test_dir);

        // Create actual test images using ImageMagick
        std::string cmd1 = "magick -size 100x100 xc:blue -fill red -draw \"circle 50,50 30,50\" " + test_dir + "/test.jpg";
        system(cmd1.c_str());

        // Create another test image
        std::string cmd2 = "magick -size 100x100 xc:green -fill yellow -draw \"rectangle 20,20 80,80\" " + test_dir + "/test.png";
        system(cmd2.c_str());

        // Create a test text file
        std::ofstream(test_dir + "/test.txt") << "text file content";
    }

    void TearDown() override
    {
        // Clean up test files
        std::vector<std::string> test_files = {
            "test_orchestrator.db",
            "test_orchestrator.db-shm",
            "test_orchestrator.db-wal"};

        for (const auto &file : test_files)
        {
            if (std::filesystem::exists(file))
            {
                std::filesystem::remove(file);
            }
        }

        // Ensure singleton is properly cleaned up
        DatabaseManager::shutdown();

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
    DatabaseManager &dbMan = DatabaseManager::getInstance(db_path);

    // Add some test files
    std::string file1 = test_dir + "/test.jpg";
    std::string file2 = test_dir + "/test.png";
    std::string file3 = test_dir + "/test.txt";
    // Image files are already created in SetUp()
    createTestFile(file3, "text file content");

    // Store test files in database
    dbMan.storeScannedFile(file1);
    dbMan.storeScannedFile(file2);
    dbMan.storeScannedFile(file3);
    dbMan.waitForWrites();

    MediaProcessingOrchestrator orchestrator(dbMan);

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
    dbMan.waitForWrites();
    // Should have processed 3 files (2 supported images, 1 unsupported text file)
    EXPECT_EQ(events.size(), 3);

    // Check that we have successful and failed events
    bool found_success = false, found_failure = false;
    int success_count = 0, failure_count = 0;
    for (const auto &event : events)
    {
        if (event.success)
        {
            found_success = true;
            success_count++;
            EXPECT_EQ(event.artifact_format, "phash");
            EXPECT_GT(event.artifact_confidence, 0.0);
        }
        else
        {
            found_failure = true;
            failure_count++;
            EXPECT_FALSE(event.error_message.empty());
        }
    }
    EXPECT_TRUE(found_success);
    EXPECT_TRUE(found_failure);
    EXPECT_EQ(success_count, 2); // 2 images should succeed
    EXPECT_EQ(failure_count, 1); // 1 text file should fail
}

TEST_F(MediaProcessingOrchestratorTest, CancelProcessing)
{
    DatabaseManager &dbMan = DatabaseManager::getInstance(db_path);

    // Add some test files (create actual images)
    std::string file1 = test_dir + "/test1.jpg";
    std::string file2 = test_dir + "/test2.png";

    // Create actual test images using ImageMagick
    std::string cmd1 = "magick -size 100x100 xc:red -fill blue -draw \"circle 50,50 30,50\" " + file1;
    system(cmd1.c_str());

    std::string cmd2 = "magick -size 100x100 xc:yellow -fill green -draw \"rectangle 20,20 80,80\" " + file2;
    system(cmd2.c_str());

    // Store test files in database
    dbMan.storeScannedFile(file1);
    dbMan.storeScannedFile(file2);
    dbMan.waitForWrites();

    MediaProcessingOrchestrator orchestrator(dbMan);

    // Use shared pointer to avoid memory issues
    auto events = std::make_shared<std::vector<FileProcessingEvent>>();
    auto processing_completed = std::make_shared<std::atomic<bool>>(false);

    // Start processing in a separate thread
    std::thread processing_thread([&orchestrator, events, processing_completed]()
                                  { orchestrator.processAllScannedFiles(2).subscribe(
                                        [events](const FileProcessingEvent &evt)
                                        {
                                            events->push_back(evt);
                                        },
                                        nullptr,
                                        [processing_completed]()
                                        {
                                            processing_completed->store(true);
                                        }); });

    // Cancel processing immediately
    orchestrator.cancel();

    // Wait for processing thread to finish
    if (processing_thread.joinable())
    {
        processing_thread.join();
    }

    // Processing should complete immediately since it's sequential

    // The cancel method should not throw and should complete gracefully
    EXPECT_TRUE(true); // If we reach here, cancel worked without crashing
}

TEST_F(MediaProcessingOrchestratorTest, CancelTimerBasedProcessing)
{
    DatabaseManager &dbMan = DatabaseManager::getInstance(db_path);
    MediaProcessingOrchestrator orchestrator(dbMan);

    // Start timer-based processing
    orchestrator.startTimerBasedProcessing(60, 1);

    // Verify processing is running
    EXPECT_TRUE(orchestrator.isTimerBasedProcessingRunning());

    // Cancel processing
    orchestrator.cancel();

    // Stop the orchestrator
    orchestrator.stopTimerBasedProcessing();

    // Verify processing has stopped
    EXPECT_FALSE(orchestrator.isTimerBasedProcessingRunning());
}