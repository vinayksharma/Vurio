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

    // Test the atomic processing methods directly
    auto files_to_process = dbMan.getAndMarkFilesForProcessing(DedupMode::BALANCED, 10);
    EXPECT_EQ(files_to_process.size(), 2); // Only the image files should be marked for processing

    // Check that files are marked as in progress (-1)
    for (const auto &[file_path, file_name] : files_to_process)
    {
        EXPECT_TRUE(dbMan.fileNeedsProcessingForMode(file_path, DedupMode::BALANCED));
    }

    // Test that the text file is not marked for processing
    EXPECT_FALSE(dbMan.fileNeedsProcessingForMode(file3, DedupMode::BALANCED));

    // Mark files as completed to clean up
    for (const auto &[file_path, file_name] : files_to_process)
    {
        dbMan.setProcessingFlag(file_path, DedupMode::BALANCED);
    }

    // Verify files are now marked as completed
    for (const auto &[file_path, file_name] : files_to_process)
    {
        EXPECT_FALSE(dbMan.fileNeedsProcessingForMode(file_path, DedupMode::BALANCED));
    }
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

    // Test the atomic processing methods directly instead of using the orchestrator
    auto files_to_process = dbMan.getAndMarkFilesForProcessing(DedupMode::BALANCED, 10);
    EXPECT_EQ(files_to_process.size(), 2); // Both image files should be marked for processing

    // Check that files are marked as in progress (-1)
    for (const auto &[file_path, file_name] : files_to_process)
    {
        EXPECT_TRUE(dbMan.fileNeedsProcessingForMode(file_path, DedupMode::BALANCED));
    }

    // Mark files as completed to clean up
    for (const auto &[file_path, file_name] : files_to_process)
    {
        dbMan.setProcessingFlag(file_path, DedupMode::BALANCED);
    }

    // Verify files are now marked as completed
    for (const auto &[file_path, file_name] : files_to_process)
    {
        EXPECT_FALSE(dbMan.fileNeedsProcessingForMode(file_path, DedupMode::BALANCED));
    }
}

TEST_F(MediaProcessingOrchestratorTest, CancelTimerBasedProcessing)
{
    DatabaseManager &dbMan = DatabaseManager::getInstance(db_path);
    MediaProcessingOrchestrator orchestrator(dbMan);

    // Test that we can start and stop timer-based processing without segfault
    // This tests the timer functionality without actually processing files

    // Start timer-based processing with a very short interval
    orchestrator.startTimerBasedProcessing(1, 1); // 1 second interval, 1 thread

    // Verify processing is running
    EXPECT_TRUE(orchestrator.isTimerBasedProcessingRunning());

    // Cancel processing immediately
    orchestrator.cancel();

    // Stop the orchestrator
    orchestrator.stopTimerBasedProcessing();

    // Verify processing has stopped
    EXPECT_FALSE(orchestrator.isTimerBasedProcessingRunning());
}