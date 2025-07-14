#include <gtest/gtest.h>
#include "core/database_manager.hpp"
#include "core/file_utils.hpp"
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>

namespace fs = std::filesystem;

class DatabaseManagerTest : public ::testing::Test
{
protected:
    std::string db_path = "test_database_manager.db";

    void SetUp() override
    {
        // Remove any existing test DB
        if (fs::exists(db_path))
            fs::remove(db_path);
    }

    void TearDown() override
    {
        if (fs::exists(db_path))
            fs::remove(db_path);
    }

    void createTestFile(const std::string &path, const std::string &content = "test content")
    {
        std::ofstream file(path);
        file << content;
        file.close();
    }
};

TEST_F(DatabaseManagerTest, DatabaseInitialization)
{
    DatabaseManager db(db_path);

    // Database should be created
    EXPECT_TRUE(fs::exists(db_path));

    // Database should be accessible
    EXPECT_NO_THROW({
        auto files = db.getFilesNeedingProcessing();
        EXPECT_EQ(files.size(), 0); // Should be empty initially
    });
}

TEST_F(DatabaseManagerTest, StoreScannedFileNewFile)
{
    DatabaseManager db(db_path);

    // Create a test file
    std::string test_file = "test_file.jpg";
    createTestFile(test_file);

    // Store the file
    db.storeScannedFile(test_file);

    // Check that file was stored with NULL hash (needs processing)
    auto files_needing_processing = db.getFilesNeedingProcessing();
    EXPECT_EQ(files_needing_processing.size(), 1);
    EXPECT_EQ(files_needing_processing[0].first, test_file);

    // Clean up test file
    fs::remove(test_file);
}

TEST_F(DatabaseManagerTest, StoreScannedFileExistingFileSameHash)
{
    DatabaseManager db(db_path);

    // Create a test file with specific content
    std::string test_file = "test_file.jpg";
    createTestFile(test_file, "specific test content for hash comparison");

    // Store the file first time
    db.storeScannedFile(test_file);

    // Get the actual hash of the file
    std::string actual_hash = FileUtils::computeFileHash(test_file);

    // Simulate processing by setting a hash
    db.updateFileHash(test_file, actual_hash);

    // Store the same file again (should not change hash since content is same)
    db.storeScannedFile(test_file);

    // Check that file still has hash (doesn't need processing)
    auto files_needing_processing = db.getFilesNeedingProcessing();
    EXPECT_EQ(files_needing_processing.size(), 0);

    // Clean up test file
    fs::remove(test_file);
}

TEST_F(DatabaseManagerTest, StoreScannedFileExistingFileDifferentHash)
{
    DatabaseManager db(db_path);

    // Create a test file
    std::string test_file = "test_file.jpg";
    createTestFile(test_file);

    // Store the file first time
    db.storeScannedFile(test_file);

    // Simulate processing by setting a hash
    db.updateFileHash(test_file, "old_hash_123");

    // Modify the file content (simulating file change)
    createTestFile(test_file, "different content");

    // Store the file again (should clear hash due to content change)
    db.storeScannedFile(test_file);

    // Check that file needs processing again (hash cleared)
    auto files_needing_processing = db.getFilesNeedingProcessing();
    EXPECT_EQ(files_needing_processing.size(), 1);
    EXPECT_EQ(files_needing_processing[0].first, test_file);

    // Clean up test file
    fs::remove(test_file);
}

TEST_F(DatabaseManagerTest, GetFilesNeedingProcessing)
{
    DatabaseManager db(db_path);

    // Create test files
    std::string file1 = "file1.jpg";
    std::string file2 = "file2.png";
    std::string file3 = "file3.mp4";

    createTestFile(file1);
    createTestFile(file2);
    createTestFile(file3);

    // Store all files
    db.storeScannedFile(file1);
    db.storeScannedFile(file2);
    db.storeScannedFile(file3);

    // All files should need processing initially
    auto files_needing_processing = db.getFilesNeedingProcessing();
    EXPECT_EQ(files_needing_processing.size(), 3);

    // Process one file (set hash)
    db.updateFileHash(file1, "hash_123");

    // Only 2 files should need processing now
    files_needing_processing = db.getFilesNeedingProcessing();
    EXPECT_EQ(files_needing_processing.size(), 2);

    // Check that file1 is not in the list
    auto it = std::find_if(files_needing_processing.begin(), files_needing_processing.end(),
                           [&file1](const std::pair<std::string, std::string> &p)
                           {
                               return p.first == file1;
                           });
    EXPECT_EQ(it, files_needing_processing.end());

    // Clean up test files
    fs::remove(file1);
    fs::remove(file2);
    fs::remove(file3);
}

TEST_F(DatabaseManagerTest, UpdateFileHash)
{
    DatabaseManager db(db_path);

    // Create a test file
    std::string test_file = "test_file.jpg";
    createTestFile(test_file);

    // Store the file
    db.storeScannedFile(test_file);

    // Initially should need processing
    auto files_needing_processing = db.getFilesNeedingProcessing();
    EXPECT_EQ(files_needing_processing.size(), 1);

    // Update hash (simulate processing)
    db.updateFileHash(test_file, "processed_hash_456");

    // Should no longer need processing
    files_needing_processing = db.getFilesNeedingProcessing();
    EXPECT_EQ(files_needing_processing.size(), 0);

    // Clean up test file
    fs::remove(test_file);
}

TEST_F(DatabaseManagerTest, StoreScannedFileWithCallback)
{
    DatabaseManager db(db_path);

    // Create a test file
    std::string test_file = "test_file.jpg";
    createTestFile(test_file);

    bool callback_called = false;
    std::string callback_file;

    // Store file with callback
    db.storeScannedFile(test_file, [&](const std::string &file_path)
                        {
        callback_called = true;
        callback_file = file_path; });

    // Wait a bit for async callback to execute
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Callback should be called for new file
    EXPECT_TRUE(callback_called);
    EXPECT_EQ(callback_file, test_file);

    // Clean up test file
    fs::remove(test_file);
}

TEST_F(DatabaseManagerTest, StoreScannedFileWithCallbackHashCleared)
{
    DatabaseManager db(db_path);

    // Create a test file
    std::string test_file = "test_file.jpg";
    createTestFile(test_file);

    // Store file first time
    db.storeScannedFile(test_file);

    // Set hash (simulate processing)
    db.updateFileHash(test_file, "old_hash_123");

    // Modify file content
    createTestFile(test_file, "different content");

    bool callback_called = false;
    std::string callback_file;

    // Store file again with callback (hash should be cleared)
    db.storeScannedFile(test_file, [&](const std::string &file_path)
                        {
        callback_called = true;
        callback_file = file_path; });

    // Wait a bit for async callback to execute
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Callback should be called when hash is cleared
    EXPECT_TRUE(callback_called);
    EXPECT_EQ(callback_file, test_file);

    // Clean up test file
    fs::remove(test_file);
}

TEST_F(DatabaseManagerTest, StoreScannedFileWithCallbackNoChange)
{
    DatabaseManager db(db_path);

    // Create a test file
    std::string test_file = "test_file.jpg";
    createTestFile(test_file);

    // Store file first time
    db.storeScannedFile(test_file);

    // Set hash (simulate processing)
    db.updateFileHash(test_file, "old_hash_123");

    bool callback_called = false;

    // Store file again with callback (no change in content)
    db.storeScannedFile(test_file, [&](const std::string &file_path)
                        { callback_called = true; });

    // Callback should NOT be called when hash doesn't change
    EXPECT_FALSE(callback_called);

    // Clean up test file
    fs::remove(test_file);
}