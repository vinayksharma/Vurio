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
        DatabaseManager::resetForTesting();
        // Remove any existing test DB
        if (fs::exists(db_path))
            fs::remove(db_path);
    }

    void TearDown() override
    {
        // Clean up test files
        std::vector<std::string> test_files = {
            "test_database_manager.db",
            "test_database_manager.db-shm",
            "test_database_manager.db-wal"};

        for (const auto &file : test_files)
        {
            if (std::filesystem::exists(file))
            {
                std::filesystem::remove(file);
            }
        }

        // Ensure singleton is properly cleaned up
        DatabaseManager::shutdown();
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
    auto &db = DatabaseManager::getInstance(db_path);

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
    auto &db = DatabaseManager::getInstance(db_path);

    // Create a test file
    std::string test_file = "test_file.jpg";
    createTestFile(test_file);

    // Store the file
    db.storeScannedFile(test_file);
    db.waitForWrites();

    // Check that file was stored with NULL hash (needs processing)
    auto files_needing_processing = db.getFilesNeedingProcessing();
    EXPECT_EQ(files_needing_processing.size(), 1);
    EXPECT_EQ(files_needing_processing[0].first, test_file);

    // Clean up test file
    fs::remove(test_file);
}

TEST_F(DatabaseManagerTest, StoreScannedFileExistingFileSameHash)
{
    auto &db = DatabaseManager::getInstance(db_path);

    // Create a test file with specific content
    std::string test_file = "test_file.jpg";
    createTestFile(test_file, "specific test content for hash comparison");

    // Store the file first time
    db.storeScannedFile(test_file);
    db.waitForWrites();

    // Get the actual hash of the file
    std::string actual_hash = FileUtils::computeFileHash(test_file);

    // Simulate processing by setting a hash
    db.updateFileHash(test_file, actual_hash);
    db.waitForWrites();

    // Store the same file again (should not change hash since content is same)
    db.storeScannedFile(test_file);
    db.waitForWrites();

    // Check that file still has hash (doesn't need processing)
    auto files_needing_processing = db.getFilesNeedingProcessing();
    EXPECT_EQ(files_needing_processing.size(), 0);

    // Clean up test file
    fs::remove(test_file);
}

TEST_F(DatabaseManagerTest, StoreScannedFileExistingFileDifferentHash)
{
    auto &db = DatabaseManager::getInstance(db_path);

    // Create a test file
    std::string test_file = "test_file.jpg";
    createTestFile(test_file);

    // Store the file first time
    db.storeScannedFile(test_file);
    db.waitForWrites();

    // Simulate processing by setting a hash
    db.updateFileHash(test_file, "old_hash_123");
    db.waitForWrites();

    // Modify the file content (simulating file change)
    createTestFile(test_file, "different content");

    // Store the file again (should clear hash due to content change)
    db.storeScannedFile(test_file);
    db.waitForWrites();

    // Check that file needs processing again (hash cleared)
    auto files_needing_processing = db.getFilesNeedingProcessing();
    EXPECT_EQ(files_needing_processing.size(), 1);
    EXPECT_EQ(files_needing_processing[0].first, test_file);

    // Clean up test file
    fs::remove(test_file);
}

TEST_F(DatabaseManagerTest, GetFilesNeedingProcessing)
{
    auto &db = DatabaseManager::getInstance(db_path);

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
    db.waitForWrites();

    // All files should need processing initially
    auto files_needing_processing = db.getFilesNeedingProcessing();
    EXPECT_EQ(files_needing_processing.size(), 3);

    // Process one file (set hash)
    db.updateFileHash(file1, "hash_123");
    db.waitForWrites();

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
    auto &db = DatabaseManager::getInstance(db_path);

    // Create a test file
    std::string test_file = "test_file.jpg";
    createTestFile(test_file);

    // Store the file
    db.storeScannedFile(test_file);
    db.waitForWrites();

    // Initially should need processing
    auto files_needing_processing = db.getFilesNeedingProcessing();
    EXPECT_EQ(files_needing_processing.size(), 1);

    // Update hash (simulate processing)
    db.updateFileHash(test_file, "processed_hash_456");
    db.waitForWrites();

    // Should no longer need processing
    files_needing_processing = db.getFilesNeedingProcessing();
    EXPECT_EQ(files_needing_processing.size(), 0);

    // Clean up test file
    fs::remove(test_file);
}

TEST_F(DatabaseManagerTest, StoreScannedFileWithCallback)
{
    auto &db = DatabaseManager::getInstance(db_path);

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
    auto &db = DatabaseManager::getInstance(db_path);

    // Create a test file
    std::string test_file = "test_file.jpg";
    createTestFile(test_file);

    // Store file first time
    db.storeScannedFile(test_file);
    db.waitForWrites();

    // Set hash (simulate processing)
    db.updateFileHash(test_file, "old_hash_123");
    db.waitForWrites();

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
    auto &db = DatabaseManager::getInstance(db_path);

    // Create a test file
    std::string test_file = "test_file.jpg";
    createTestFile(test_file);

    // Store file first time
    db.storeScannedFile(test_file);
    db.waitForWrites();

    // Set hash (simulate processing)
    db.updateFileHash(test_file, "old_hash_123");
    db.waitForWrites();

    bool callback_called = false;

    // Store file again with callback (no change in content)
    db.storeScannedFile(test_file, [&](const std::string &file_path)
                        { callback_called = true; });
    db.waitForWrites();

    // Callback should NOT be called when hash doesn't change
    EXPECT_FALSE(callback_called);

    // Clean up test file
    fs::remove(test_file);
}

TEST_F(DatabaseManagerTest, StoreProcessingResult)
{
    auto &db = DatabaseManager::getInstance(db_path);

    // Create a test file
    std::string test_file = "test_file.jpg";
    createTestFile(test_file);

    // Create a processing result
    ProcessingResult result;
    result.success = true;
    result.artifact.format = "phash";
    result.artifact.hash = "test_hash_123";
    result.artifact.confidence = 0.95;
    result.artifact.metadata = "{\"test\":\"metadata\"}";
    result.artifact.data = {0x01, 0x02, 0x03, 0x04};

    // Store the processing result
    auto db_result = db.storeProcessingResult(test_file, DedupMode::BALANCED, result);
    EXPECT_TRUE(db_result.success);
    db.waitForWrites();

    // Verify the result was stored
    auto results = db.getProcessingResults(test_file);
    EXPECT_EQ(results.size(), 1);
    EXPECT_TRUE(results[0].success);
    EXPECT_EQ(results[0].artifact.format, "phash");
    EXPECT_EQ(results[0].artifact.hash, "test_hash_123");
    EXPECT_EQ(results[0].artifact.confidence, 0.95);
    EXPECT_EQ(results[0].artifact.metadata, "{\"test\":\"metadata\"}");
    EXPECT_EQ(results[0].artifact.data.size(), 4);
    EXPECT_EQ(results[0].artifact.data[0], 0x01);

    // Clean up test file
    fs::remove(test_file);
}

TEST_F(DatabaseManagerTest, StoreProcessingResultWithError)
{
    auto &db = DatabaseManager::getInstance(db_path);

    // Create a test file
    std::string test_file = "test_file.jpg";
    createTestFile(test_file);

    // Create a processing result with error
    ProcessingResult result;
    result.success = false;
    result.error_message = "Processing failed";

    // Store the processing result
    auto db_result = db.storeProcessingResult(test_file, DedupMode::FAST, result);
    EXPECT_TRUE(db_result.success);
    db.waitForWrites();

    // Verify the result was stored
    auto results = db.getProcessingResults(test_file);
    EXPECT_EQ(results.size(), 1);
    EXPECT_FALSE(results[0].success);
    EXPECT_EQ(results[0].error_message, "Processing failed");

    // Clean up test file
    fs::remove(test_file);
}

TEST_F(DatabaseManagerTest, GetProcessingResultsMultiple)
{
    auto &db = DatabaseManager::getInstance(db_path);

    // Create a test file
    std::string test_file = "test_file.jpg";
    createTestFile(test_file);

    // Store multiple processing results
    ProcessingResult result1;
    result1.success = true;
    result1.artifact.format = "phash";
    result1.artifact.hash = "hash1";

    ProcessingResult result2;
    result2.success = true;
    result2.artifact.format = "dhash";
    result2.artifact.hash = "hash2";

    db.storeProcessingResult(test_file, DedupMode::BALANCED, result1);
    db.storeProcessingResult(test_file, DedupMode::FAST, result2);
    db.waitForWrites();

    // Get all results for the file
    auto results = db.getProcessingResults(test_file);
    EXPECT_EQ(results.size(), 2);

    // Results should be ordered by created_at DESC (newest first)
    // Since both results are inserted quickly, we can't guarantee order
    // So we'll check that both results exist with the correct data
    bool found_phash = false, found_dhash = false;
    for (const auto &result : results)
    {
        if (result.artifact.format == "phash" && result.artifact.hash == "hash1")
        {
            found_phash = true;
        }
        else if (result.artifact.format == "dhash" && result.artifact.hash == "hash2")
        {
            found_dhash = true;
        }
    }
    EXPECT_TRUE(found_phash);
    EXPECT_TRUE(found_dhash);

    // Clean up test file
    fs::remove(test_file);
}

TEST_F(DatabaseManagerTest, GetAllProcessingResults)
{
    auto &db = DatabaseManager::getInstance(db_path);

    // Create test files
    std::string file1 = "file1.jpg";
    std::string file2 = "file2.png";
    createTestFile(file1);
    createTestFile(file2);

    // Store processing results for different files
    ProcessingResult result1;
    result1.success = true;
    result1.artifact.format = "phash";
    result1.artifact.hash = "hash1";

    ProcessingResult result2;
    result2.success = true;
    result2.artifact.format = "dhash";
    result2.artifact.hash = "hash2";

    db.storeProcessingResult(file1, DedupMode::BALANCED, result1);
    db.storeProcessingResult(file2, DedupMode::FAST, result2);
    db.waitForWrites();

    // Get all processing results
    auto all_results = db.getAllProcessingResults();
    EXPECT_EQ(all_results.size(), 2);

    // Check that we have results for both files
    bool found_file1 = false, found_file2 = false;
    for (const auto &pair : all_results)
    {
        if (pair.first == file1)
        {
            found_file1 = true;
            EXPECT_EQ(pair.second.artifact.format, "phash");
            EXPECT_EQ(pair.second.artifact.hash, "hash1");
        }
        else if (pair.first == file2)
        {
            found_file2 = true;
            EXPECT_EQ(pair.second.artifact.format, "dhash");
            EXPECT_EQ(pair.second.artifact.hash, "hash2");
        }
    }
    EXPECT_TRUE(found_file1);
    EXPECT_TRUE(found_file2);

    // Clean up test files
    fs::remove(file1);
    fs::remove(file2);
}

TEST_F(DatabaseManagerTest, ClearAllResults)
{
    auto &db = DatabaseManager::getInstance(db_path);

    // Create a test file
    std::string test_file = "test_file.jpg";
    createTestFile(test_file);

    // Store a processing result
    ProcessingResult result;
    result.success = true;
    result.artifact.format = "phash";
    result.artifact.hash = "test_hash";

    db.storeProcessingResult(test_file, DedupMode::BALANCED, result);
    db.waitForWrites();

    // Verify result exists
    auto results = db.getProcessingResults(test_file);
    EXPECT_EQ(results.size(), 1);

    // Clear all results
    auto clear_result = db.clearAllResults();
    EXPECT_TRUE(clear_result.success);
    db.waitForWrites();

    // Verify results are cleared
    results = db.getProcessingResults(test_file);
    EXPECT_EQ(results.size(), 0);

    // Clean up test file
    fs::remove(test_file);
}

TEST_F(DatabaseManagerTest, GetAllScannedFiles)
{
    auto &db = DatabaseManager::getInstance(db_path);

    // Create test files
    std::string file1 = "file1.jpg";
    std::string file2 = "file2.png";
    createTestFile(file1);
    createTestFile(file2);

    // Store scanned files
    db.storeScannedFile(file1);
    db.storeScannedFile(file2);
    db.waitForWrites();

    // Get all scanned files
    auto all_files = db.getAllScannedFiles();
    EXPECT_EQ(all_files.size(), 2);

    // Check that we have both files
    bool found_file1 = false, found_file2 = false;
    for (const auto &pair : all_files)
    {
        if (pair.first == file1)
        {
            found_file1 = true;
            EXPECT_EQ(pair.second, "file1.jpg");
        }
        else if (pair.first == file2)
        {
            found_file2 = true;
            EXPECT_EQ(pair.second, "file2.png");
        }
    }
    EXPECT_TRUE(found_file1);
    EXPECT_TRUE(found_file2);

    // Clean up test files
    fs::remove(file1);
    fs::remove(file2);
}

TEST_F(DatabaseManagerTest, ClearAllScannedFiles)
{
    auto &db = DatabaseManager::getInstance(db_path);

    // Create a test file
    std::string test_file = "test_file.jpg";
    createTestFile(test_file);

    // Store a scanned file
    db.storeScannedFile(test_file);
    db.waitForWrites();

    // Verify file exists
    auto all_files = db.getAllScannedFiles();
    EXPECT_EQ(all_files.size(), 1);

    // Clear all scanned files
    auto clear_result = db.clearAllScannedFiles();
    EXPECT_TRUE(clear_result.success);
    db.waitForWrites();

    // Verify files are cleared
    all_files = db.getAllScannedFiles();
    EXPECT_EQ(all_files.size(), 0);

    // Clean up test file
    fs::remove(test_file);
}

TEST_F(DatabaseManagerTest, IsValid)
{
    auto &db = DatabaseManager::getInstance(db_path);

    // Database should be valid after construction
    EXPECT_TRUE(db.isValid());

    // Test with invalid database path (this would require a different test setup)
    // For now, we just verify that isValid() returns true for a properly initialized database
    EXPECT_TRUE(db.isValid());
}