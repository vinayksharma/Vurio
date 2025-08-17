#include "database/database_manager.hpp"
#include <gtest/gtest.h>
#include "core/file_utils.hpp"
#include "core/dedup_modes.hpp"
#include "logging/logger.hpp"
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <iostream> // Added for debug output

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
    auto &dbMan = DatabaseManager::getInstance(db_path);

    // Database should be created
    EXPECT_TRUE(fs::exists(db_path));

    // Database should be accessible
    EXPECT_NO_THROW({
        // Test getFilesNeedingProcessing
        auto files = dbMan.getFilesNeedingProcessing(DedupMode::BALANCED);
        EXPECT_EQ(files.size(), 0); // Should be empty initially
    });
}

TEST_F(DatabaseManagerTest, StoreScannedFileNewFile)
{
    auto &dbMan = DatabaseManager::getInstance(db_path);

    // Create a test file
    std::string test_file = "test_file.jpg";
    createTestFile(test_file);

    // Store the file
    dbMan.storeScannedFile(test_file);
    dbMan.waitForWrites();

    // Check that file was stored with NULL hash (needs processing)
    auto files_needing_processing = dbMan.getFilesNeedingProcessing(DedupMode::BALANCED);
    EXPECT_EQ(files_needing_processing.size(), 1);
    EXPECT_EQ(files_needing_processing[0].first, test_file);

    // Clean up test file
    fs::remove(test_file);
}

TEST_F(DatabaseManagerTest, StoreScannedFileExistingFileSameMetadata)
{
    auto &dbMan = DatabaseManager::getInstance(db_path);

    // Create a test file
    std::string test_file = "test_file.jpg";
    createTestFile(test_file);

    // Store the file first time
    dbMan.storeScannedFile(test_file);
    dbMan.waitForWrites();

    // Process the file to set the processing flag
    ProcessingResult result;
    result.success = true;
    result.artifact.format = "phash";
    result.artifact.hash = "test_hash";
    result.artifact.confidence = 0.95;
    dbMan.storeProcessingResult(test_file, DedupMode::BALANCED, result);
    dbMan.setProcessingFlag(test_file, DedupMode::BALANCED);
    dbMan.waitForWrites();

    // Store the same file again (should not change metadata since content is same)
    dbMan.storeScannedFile(test_file);
    dbMan.waitForWrites();

    // Check that file no longer needs processing (has processing flag set)
    auto files_needing_processing = dbMan.getFilesNeedingProcessing(DedupMode::BALANCED);
    EXPECT_EQ(files_needing_processing.size(), 0);

    // Clean up test file
    fs::remove(test_file);
}

TEST_F(DatabaseManagerTest, StoreScannedFileExistingFileDifferentMetadata)
{
    auto &dbMan = DatabaseManager::getInstance(db_path);

    // Create a test file
    std::string test_file = "test_file.jpg";
    createTestFile(test_file);

    // Store the file first time
    dbMan.storeScannedFile(test_file);
    dbMan.waitForWrites();

    // Simulate processing by setting metadata (simulate mismatch)
    dbMan.updateFileMetadata(test_file, "old_metadata_123");
    dbMan.waitForWrites();

    // Modify the file content (simulating file change)
    createTestFile(test_file, "different content");

    // Store the file again (should clear metadata due to content change)
    dbMan.storeScannedFile(test_file);
    dbMan.waitForWrites();

    // Check that file needs processing again (metadata cleared)
    auto files_needing_processing = dbMan.getFilesNeedingProcessing(DedupMode::BALANCED);
    EXPECT_EQ(files_needing_processing.size(), 1);
    EXPECT_EQ(files_needing_processing[0].first, test_file);

    // Clean up test file
    fs::remove(test_file);
}

TEST_F(DatabaseManagerTest, GetFilesNeedingProcessing)
{
    auto &dbMan = DatabaseManager::getInstance(db_path);

    // Create test files
    std::string file1 = "file1.jpg";
    std::string file2 = "file2.png";
    std::string file3 = "file3.mp4";
    createTestFile(file1);
    createTestFile(file2);
    createTestFile(file3);

    // Store files
    dbMan.storeScannedFile(file1);
    dbMan.storeScannedFile(file2);
    dbMan.storeScannedFile(file3);
    dbMan.waitForWrites();

    // Initially all files need processing
    auto files_needing_processing = dbMan.getFilesNeedingProcessing(DedupMode::BALANCED);
    EXPECT_EQ(files_needing_processing.size(), 3);

    // Process file1 and set processing flag
    ProcessingResult result1;
    result1.success = true;
    result1.artifact.format = "phash";
    result1.artifact.hash = "test_hash_1";
    result1.artifact.confidence = 0.95;
    dbMan.storeProcessingResult(file1, DedupMode::BALANCED, result1);
    dbMan.setProcessingFlag(file1, DedupMode::BALANCED);
    dbMan.waitForWrites();

    // Now only file2 and file3 need processing
    files_needing_processing = dbMan.getFilesNeedingProcessing(DedupMode::BALANCED);
    EXPECT_EQ(files_needing_processing.size(), 2);

    // Verify file1 is not in the list
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

TEST_F(DatabaseManagerTest, UpdateFileMetadata)
{
    auto &dbMan = DatabaseManager::getInstance(db_path);

    // Create a test file
    std::string test_file = "test_file.jpg";
    createTestFile(test_file);

    // Store the file
    dbMan.storeScannedFile(test_file);
    dbMan.waitForWrites();

    // Process the file and set processing flag
    ProcessingResult result;
    result.success = true;
    result.artifact.format = "phash";
    result.artifact.hash = "test_hash";
    result.artifact.confidence = 0.95;
    dbMan.storeProcessingResult(test_file, DedupMode::BALANCED, result);
    dbMan.setProcessingFlag(test_file, DedupMode::BALANCED);
    dbMan.waitForWrites();

    // Check that file no longer needs processing
    auto files_needing_processing = dbMan.getFilesNeedingProcessing(DedupMode::BALANCED);
    EXPECT_EQ(files_needing_processing.size(), 0);

    // Clean up test file
    fs::remove(test_file);
}

TEST_F(DatabaseManagerTest, StoreScannedFileWithCallback)
{
    auto &dbMan = DatabaseManager::getInstance(db_path);

    // Create a test file
    std::string test_file = "test_file.jpg";
    createTestFile(test_file);

    bool callback_called = false;
    std::string callback_file;

    // Store file with callback
    dbMan.storeScannedFile(test_file, [&](const std::string &file_path)
                           {
        callback_called = true;
        callback_file = file_path; });

    // Callback is executed synchronously within the database operation
    // No need to wait

    // Callback should be called for new file
    EXPECT_TRUE(callback_called);
    EXPECT_EQ(callback_file, test_file);

    // Clean up test file
    fs::remove(test_file);
}

TEST_F(DatabaseManagerTest, StoreScannedFileWithCallbackMetadataCleared)
{
    auto &dbMan = DatabaseManager::getInstance(db_path);

    // Create a test file
    std::string test_file = "test_file.jpg";
    createTestFile(test_file);

    // Store file first time
    dbMan.storeScannedFile(test_file);
    dbMan.waitForWrites();

    // Set metadata (simulate processing)
    dbMan.updateFileMetadata(test_file, "old_metadata_123");
    dbMan.waitForWrites();

    // Modify file content
    createTestFile(test_file, "different content");

    bool callback_called = false;
    std::string callback_file;

    // Store file again with callback (metadata should be cleared)
    dbMan.storeScannedFile(test_file, [&](const std::string &file_path)
                           {
        callback_called = true;
        callback_file = file_path; });

    // Callback is executed synchronously within the database operation
    // No need to wait

    // Callback should be called when metadata is cleared
    EXPECT_TRUE(callback_called);
    EXPECT_EQ(callback_file, test_file);

    // Clean up test file
    fs::remove(test_file);
}

TEST_F(DatabaseManagerTest, StoreScannedFileWithCallbackNoChange)
{
    auto &dbMan = DatabaseManager::getInstance(db_path);

    // Create a test file
    std::string test_file = "test_file.jpg";
    createTestFile(test_file);

    // Store file first time
    dbMan.storeScannedFile(test_file);
    dbMan.waitForWrites();

    // Set metadata (simulate processing)
    auto metadata = FileUtils::getFileMetadata(test_file);
    EXPECT_TRUE(metadata.has_value());
    std::string actual_metadata_str = FileUtils::metadataToString(*metadata);
    // Debug output to help diagnose the issue
    Logger::debug("Actual file metadata: " + actual_metadata_str);
    dbMan.updateFileMetadata(test_file, actual_metadata_str);
    dbMan.waitForWrites();

    // Retrieve metadata from DB for debug
    auto files = dbMan.getAllScannedFiles();
    for (const auto &pair : files)
    {
        if (pair.first == test_file)
        {
            // Debug output
            Logger::debug("DB file: " + pair.first + ", DB metadata: " + FileUtils::getFileMetadata(pair.first)->toString());
        }
    }

    bool callback_called = false;

    // Store file again with callback (no change in content)
    auto metadata_before_second_store = FileUtils::getFileMetadata(test_file);
    // Debug output
    Logger::debug("Metadata before second store: " + metadata_before_second_store->toString());
    dbMan.storeScannedFile(test_file, [&](const std::string &file_path)
                           { callback_called = true; });
    dbMan.waitForWrites();

    // Callback is executed synchronously within the database operation
    // No need to wait (callback should NOT be called)

    // Callback should NOT be called when metadata doesn't change
    EXPECT_FALSE(callback_called);

    // Clean up test file
    fs::remove(test_file);
}

TEST_F(DatabaseManagerTest, StoreProcessingResult)
{
    auto &dbMan = DatabaseManager::getInstance(db_path);

    // Create a test file
    std::string test_file = "test_file.jpg";
    createTestFile(test_file);

    // First, store the file in scanned_files table
    auto scan_result = dbMan.storeScannedFile(test_file);
    EXPECT_TRUE(scan_result.success);
    dbMan.waitForWrites();

    // Create a processing result
    ProcessingResult result;
    result.success = true;
    result.artifact.format = "phash";
    result.artifact.hash = "test_hash_123";
    result.artifact.confidence = 0.95;
    result.artifact.metadata = "{\"test\":\"metadata\"}";
    result.artifact.data = {0x01, 0x02, 0x03, 0x04};

    // Store the processing result
    auto db_result = dbMan.storeProcessingResult(test_file, DedupMode::BALANCED, result);
    EXPECT_TRUE(db_result.success);
    dbMan.waitForWrites();

    // Verify the result was stored
    auto results = dbMan.getProcessingResults(test_file);
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
    auto &dbMan = DatabaseManager::getInstance(db_path);

    // Create a test file
    std::string test_file = "test_file.jpg";
    createTestFile(test_file);

    // First, store the file in scanned_files table
    auto scan_result = dbMan.storeScannedFile(test_file);
    EXPECT_TRUE(scan_result.success);
    dbMan.waitForWrites();

    // Create a processing result with error
    ProcessingResult result;
    result.success = false;

    // Store the processing result
    auto db_result = dbMan.storeProcessingResult(test_file, DedupMode::FAST, result);
    EXPECT_TRUE(db_result.success);
    dbMan.waitForWrites();

    // Verify the result was stored
    auto results = dbMan.getProcessingResults(test_file);
    EXPECT_EQ(results.size(), 1);
    EXPECT_FALSE(results[0].success);

    // Clean up test file
    fs::remove(test_file);
}

TEST_F(DatabaseManagerTest, GetProcessingResultsMultiple)
{
    auto &dbMan = DatabaseManager::getInstance(db_path);

    // Create a test file
    std::string test_file = "test_file.jpg";
    createTestFile(test_file);

    // First, store the file in scanned_files table
    auto scan_result = dbMan.storeScannedFile(test_file);
    EXPECT_TRUE(scan_result.success);
    dbMan.waitForWrites();

    // Store processing results for different modes
    ProcessingResult result1;
    result1.success = true;
    result1.artifact.format = "phash";
    result1.artifact.hash = "hash1";

    ProcessingResult result2;
    result2.success = true;
    result2.artifact.format = "dhash";
    result2.artifact.hash = "hash2";

    // Store results for different modes (should create separate records)
    dbMan.storeProcessingResult(test_file, DedupMode::BALANCED, result1);
    dbMan.storeProcessingResult(test_file, DedupMode::FAST, result2);
    dbMan.waitForWrites();

    // Get all results for the file
    auto results = dbMan.getProcessingResults(test_file);
    EXPECT_EQ(results.size(), 2); // Should have one record per mode

    // Check that both results exist with the correct data
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

    // Test that storing the same mode again replaces the previous result
    ProcessingResult result3;
    result3.success = true;
    result3.artifact.format = "phash_updated";
    result3.artifact.hash = "hash3";

    dbMan.storeProcessingResult(test_file, DedupMode::BALANCED, result3);
    dbMan.waitForWrites();

    // Should still have 2 results (one per mode), but BALANCED should be updated
    results = dbMan.getProcessingResults(test_file);
    EXPECT_EQ(results.size(), 2);

    // Check that the BALANCED mode result was updated
    bool found_updated_phash = false;
    for (const auto &result : results)
    {
        if (result.artifact.format == "phash_updated" && result.artifact.hash == "hash3")
        {
            found_updated_phash = true;
        }
    }
    EXPECT_TRUE(found_updated_phash);

    // Clean up test file
    fs::remove(test_file);
}

TEST_F(DatabaseManagerTest, GetAllProcessingResults)
{
    auto &dbMan = DatabaseManager::getInstance(db_path);

    // Create test files
    std::string file1 = "file1.jpg";
    std::string file2 = "file2.png";
    createTestFile(file1);
    createTestFile(file2);

    // First, store the files in scanned_files table
    auto scan_result1 = dbMan.storeScannedFile(file1);
    EXPECT_TRUE(scan_result1.success);
    auto scan_result2 = dbMan.storeScannedFile(file2);
    EXPECT_TRUE(scan_result2.success);
    dbMan.waitForWrites();

    // Store processing results for different files
    ProcessingResult result1;
    result1.success = true;
    result1.artifact.format = "phash";
    result1.artifact.hash = "hash1";

    ProcessingResult result2;
    result2.success = true;
    result2.artifact.format = "dhash";
    result2.artifact.hash = "hash2";

    dbMan.storeProcessingResult(file1, DedupMode::BALANCED, result1);
    dbMan.storeProcessingResult(file2, DedupMode::FAST, result2);
    dbMan.waitForWrites();

    // Get all processing results
    auto all_results = dbMan.getAllProcessingResults();
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
    auto &dbMan = DatabaseManager::getInstance(db_path);

    // Create a test file
    std::string test_file = "test_file.jpg";
    createTestFile(test_file);

    // First, store the file in scanned_files table
    auto scan_result = dbMan.storeScannedFile(test_file);
    EXPECT_TRUE(scan_result.success);
    dbMan.waitForWrites();

    // Store a processing result
    ProcessingResult result;
    result.success = true;
    result.artifact.format = "phash";
    result.artifact.hash = "test_hash";

    auto db_result = dbMan.storeProcessingResult(test_file, DedupMode::BALANCED, result);
    EXPECT_TRUE(db_result.success);
    dbMan.waitForWrites();

    // Verify the result was stored
    auto results = dbMan.getProcessingResults(test_file);
    EXPECT_EQ(results.size(), 1);

    // Clear all results
    auto clear_result = dbMan.clearAllResults();
    EXPECT_TRUE(clear_result.success);
    dbMan.waitForWrites();

    // Verify all results were cleared
    results = dbMan.getProcessingResults(test_file);
    EXPECT_EQ(results.size(), 0);

    // Clean up test file
    fs::remove(test_file);
}

TEST_F(DatabaseManagerTest, GetAllScannedFiles)
{
    auto &dbMan = DatabaseManager::getInstance(db_path);

    // Create test files
    std::string file1 = "file1.jpg";
    std::string file2 = "file2.png";
    createTestFile(file1);
    createTestFile(file2);

    // Store scanned files
    dbMan.storeScannedFile(file1);
    dbMan.storeScannedFile(file2);
    dbMan.waitForWrites();

    // Get all scanned files
    auto all_files = dbMan.getAllScannedFiles();
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
    auto &dbMan = DatabaseManager::getInstance(db_path);

    // Create a test file
    std::string test_file = "test_file.jpg";
    createTestFile(test_file);

    // Store a scanned file
    dbMan.storeScannedFile(test_file);
    dbMan.waitForWrites();

    // Verify file exists
    auto all_files = dbMan.getAllScannedFiles();
    EXPECT_EQ(all_files.size(), 1);

    // Clear all scanned files
    auto clear_result = dbMan.clearAllScannedFiles();
    EXPECT_TRUE(clear_result.success);
    dbMan.waitForWrites();

    // Verify files are cleared
    all_files = dbMan.getAllScannedFiles();
    EXPECT_EQ(all_files.size(), 0);

    // Clean up test file
    fs::remove(test_file);
}

TEST_F(DatabaseManagerTest, IsValid)
{
    auto &dbMan = DatabaseManager::getInstance(db_path);

    // Database should be valid after initialization
    EXPECT_TRUE(dbMan.isValid());

    // For now, we just verify that isValid() returns true for a properly initialized database
    EXPECT_TRUE(dbMan.isValid());
}

// Links functionality tests

TEST_F(DatabaseManagerTest, SetFileLinks)
{
    auto &dbMan = DatabaseManager::getInstance(db_path);

    // Create a test file
    std::string test_file = "test_file.jpg";
    createTestFile(test_file);

    // Store the file first
    dbMan.storeScannedFile(test_file);
    dbMan.waitForWrites();

    // Set links for the file in BALANCED mode
    std::vector<int> linked_ids = {1, 2, 3, 5, 8};
    auto result = dbMan.setFileLinksForMode(test_file, linked_ids, DedupMode::BALANCED);
    EXPECT_TRUE(result.success);
    dbMan.waitForWrites();

    // Verify links were set correctly for BALANCED mode
    auto retrieved_links = dbMan.getFileLinksForMode(test_file, DedupMode::BALANCED);
    EXPECT_EQ(retrieved_links.size(), 5);
    EXPECT_EQ(retrieved_links[0], 1);
    EXPECT_EQ(retrieved_links[1], 2);
    EXPECT_EQ(retrieved_links[2], 3);
    EXPECT_EQ(retrieved_links[3], 5);
    EXPECT_EQ(retrieved_links[4], 8);

    // Clean up test file
    fs::remove(test_file);
}

TEST_F(DatabaseManagerTest, GetFileLinksEmpty)
{
    auto &dbMan = DatabaseManager::getInstance(db_path);

    // Create a test file
    std::string test_file = "test_file.jpg";
    createTestFile(test_file);

    // Store the file first
    dbMan.storeScannedFile(test_file);
    dbMan.waitForWrites();

    // Get links for file with no links set in BALANCED mode
    auto links = dbMan.getFileLinksForMode(test_file, DedupMode::BALANCED);
    EXPECT_EQ(links.size(), 0);

    // Clean up test file
    fs::remove(test_file);
}

TEST_F(DatabaseManagerTest, AddFileLink)
{
    auto &dbMan = DatabaseManager::getInstance(db_path);

    // Create a test file
    std::string test_file = "test_file.jpg";
    createTestFile(test_file);

    // Store the file first
    dbMan.storeScannedFile(test_file);
    dbMan.waitForWrites();

    // Add a link in BALANCED mode
    auto result = dbMan.addFileLink(test_file, 42);
    EXPECT_TRUE(result.success);
    dbMan.waitForWrites();

    // Verify link was added
    auto links = dbMan.getFileLinksForMode(test_file, DedupMode::BALANCED);
    EXPECT_EQ(links.size(), 1);
    EXPECT_EQ(links[0], 42);

    // Add another link
    result = dbMan.addFileLink(test_file, 99);
    EXPECT_TRUE(result.success);
    dbMan.waitForWrites();

    // Verify both links exist
    links = dbMan.getFileLinksForMode(test_file, DedupMode::BALANCED);
    EXPECT_EQ(links.size(), 2);
    EXPECT_EQ(links[0], 42);
    EXPECT_EQ(links[1], 99);

    // Try to add the same link again (should not duplicate)
    result = dbMan.addFileLink(test_file, 42);
    EXPECT_TRUE(result.success);
    dbMan.waitForWrites();

    // Verify no duplicate was added
    links = dbMan.getFileLinksForMode(test_file, DedupMode::BALANCED);
    EXPECT_EQ(links.size(), 2);
    EXPECT_EQ(links[0], 42);
    EXPECT_EQ(links[1], 99);

    // Clean up test file
    fs::remove(test_file);
}

TEST_F(DatabaseManagerTest, RemoveFileLink)
{
    auto &dbMan = DatabaseManager::getInstance(db_path);

    // Create a test file
    std::string test_file = "test_file.jpg";
    createTestFile(test_file);

    // Store the file first
    dbMan.storeScannedFile(test_file);
    dbMan.waitForWrites();

    // Set initial links in BALANCED mode
    std::vector<int> initial_links = {1, 2, 3, 4, 5};
    dbMan.setFileLinksForMode(test_file, initial_links, DedupMode::BALANCED);
    dbMan.waitForWrites();

    // Remove a link
    auto result = dbMan.removeFileLink(test_file, 3);
    EXPECT_TRUE(result.success);
    dbMan.waitForWrites();

    // Verify link was removed
    auto links = dbMan.getFileLinksForMode(test_file, DedupMode::BALANCED);
    EXPECT_EQ(links.size(), 4);
    EXPECT_EQ(links[0], 1);
    EXPECT_EQ(links[1], 2);
    EXPECT_EQ(links[2], 4);
    EXPECT_EQ(links[3], 5);

    // Try to remove a link that doesn't exist
    result = dbMan.removeFileLink(test_file, 999);
    EXPECT_TRUE(result.success); // Should not fail, just return success
    dbMan.waitForWrites();

    // Verify links are unchanged
    links = dbMan.getFileLinksForMode(test_file, DedupMode::BALANCED);
    EXPECT_EQ(links.size(), 4);

    // Clean up test file
    fs::remove(test_file);
}

TEST_F(DatabaseManagerTest, GetLinkedFiles)
{
    auto &dbMan = DatabaseManager::getInstance(db_path);

    // Create test files
    std::string file1 = "file1.jpg";
    std::string file2 = "file2.jpg";
    std::string file3 = "file3.jpg";
    createTestFile(file1);
    createTestFile(file2);
    createTestFile(file3);

    // Store all files
    dbMan.storeScannedFile(file1);
    dbMan.storeScannedFile(file2);
    dbMan.storeScannedFile(file3);
    dbMan.waitForWrites();

    // Get the ID of file1
    auto file1_links = dbMan.getFileLinksForMode(file1, DedupMode::BALANCED);
    EXPECT_EQ(file1_links.size(), 0);

    // Set file2 and file3 to link to file1 in BALANCED mode
    dbMan.addFileLink(file2, 1); // Assuming file1 has ID 1
    dbMan.addFileLink(file3, 1);
    dbMan.waitForWrites();

    // Get files linked to file1
    auto linked_files = dbMan.getLinkedFiles(file1);
    EXPECT_EQ(linked_files.size(), 2);

    // Check that both file2 and file3 are in the results
    bool found_file2 = false, found_file3 = false;
    for (const auto &linked_file : linked_files)
    {
        if (linked_file == file2)
            found_file2 = true;
        else if (linked_file == file3)
            found_file3 = true;
    }
    EXPECT_TRUE(found_file2);
    EXPECT_TRUE(found_file3);

    // Clean up test files
    fs::remove(file1);
    fs::remove(file2);
    fs::remove(file3);
}

TEST_F(DatabaseManagerTest, LinksJsonSerialization)
{
    auto &dbMan = DatabaseManager::getInstance(db_path);

    // Create a test file
    std::string test_file = "test_file.jpg";
    createTestFile(test_file);

    // Store the file first
    dbMan.storeScannedFile(test_file);
    dbMan.waitForWrites();

    // Test with various link combinations
    std::vector<std::vector<int>> test_cases = {
        {},                   // Empty array
        {1},                  // Single element
        {1, 2, 3},            // Multiple elements
        {100, 200, 300, 400}, // Large numbers
        {0, 1, 2, 3, 4, 5}    // Sequential numbers
    };

    for (const auto &test_case : test_cases)
    {
        // Set links in BALANCED mode
        auto result = dbMan.setFileLinksForMode(test_file, test_case, DedupMode::BALANCED);
        EXPECT_TRUE(result.success);
        dbMan.waitForWrites();

        // Retrieve links for BALANCED mode
        auto retrieved_links = dbMan.getFileLinksForMode(test_file, DedupMode::BALANCED);
        EXPECT_EQ(retrieved_links.size(), test_case.size());

        // Verify each link matches
        for (size_t i = 0; i < test_case.size(); ++i)
        {
            EXPECT_EQ(retrieved_links[i], test_case[i]);
        }
    }

    // Clean up test file
    fs::remove(test_file);
}

TEST_F(DatabaseManagerTest, UserInputs)
{
    auto &dbMan = DatabaseManager::getInstance(db_path);

    // Test storing user inputs
    auto result1 = dbMan.storeUserInput("scan_path", "/path/to/directory1");
    EXPECT_TRUE(result1.success);

    auto result2 = dbMan.storeUserInput("scan_path", "/path/to/directory2");
    EXPECT_TRUE(result2.success);

    auto result3 = dbMan.storeUserInput("config_setting", "quality_mode=FAST");
    EXPECT_TRUE(result3.success);

    dbMan.waitForWrites();

    // Test getting user inputs by type
    auto scan_paths = dbMan.getUserInputs("scan_path");
    EXPECT_EQ(scan_paths.size(), 2);
    EXPECT_TRUE(std::find(scan_paths.begin(), scan_paths.end(), "/path/to/directory1") != scan_paths.end());
    EXPECT_TRUE(std::find(scan_paths.begin(), scan_paths.end(), "/path/to/directory2") != scan_paths.end());

    auto config_settings = dbMan.getUserInputs("config_setting");
    EXPECT_EQ(config_settings.size(), 1);
    EXPECT_EQ(config_settings[0], "quality_mode=FAST");

    // Test getting all user inputs
    auto all_inputs = dbMan.getAllUserInputs();
    EXPECT_EQ(all_inputs.size(), 3);

    bool found_scan_path1 = false, found_scan_path2 = false, found_config = false;
    for (const auto &[input_type, input_value] : all_inputs)
    {
        if (input_type == "scan_path" && input_value == "/path/to/directory1")
            found_scan_path1 = true;
        else if (input_type == "scan_path" && input_value == "/path/to/directory2")
            found_scan_path2 = true;
        else if (input_type == "config_setting" && input_value == "quality_mode=FAST")
            found_config = true;
    }
    EXPECT_TRUE(found_scan_path1);
    EXPECT_TRUE(found_scan_path2);
    EXPECT_TRUE(found_config);

    // Test clearing all user inputs
    auto clear_result = dbMan.clearAllUserInputs();
    EXPECT_TRUE(clear_result.success);
    dbMan.waitForWrites();

    // Verify all inputs were cleared
    auto remaining_inputs = dbMan.getAllUserInputs();
    EXPECT_EQ(remaining_inputs.size(), 0);
}

TEST_F(DatabaseManagerTest, QueueInitializationRetry)
{
    auto &dbMan = DatabaseManager::getInstance(db_path);

    // Test that the queue initialization utility works correctly
    bool queue_ready = dbMan.waitForQueueInitialization(3, 100); // 3 retries, 100ms delay
    EXPECT_TRUE(queue_ready);

    // Test with custom retry parameters
    bool queue_ready2 = dbMan.waitForQueueInitialization(1, 50); // 1 retry, 50ms delay
    EXPECT_TRUE(queue_ready2);

    // Test that user input operations work after queue initialization
    auto result = dbMan.storeUserInput("test_type", "test_value");
    EXPECT_TRUE(result.success);

    auto inputs = dbMan.getUserInputs("test_type");
    EXPECT_EQ(inputs.size(), 1);
    EXPECT_EQ(inputs[0], "test_value");
}

TEST_F(DatabaseManagerTest, MetadataBasedChangeDetection)
{
    auto &dbMan = DatabaseManager::getInstance(db_path);

    // Create a test file
    std::string test_file = "test_metadata_detection.jpg";
    createTestFile(test_file);

    // Store the file (should have no metadata initially)
    dbMan.storeScannedFile(test_file);
    dbMan.waitForWrites();

    // Initially should need processing
    auto files_needing_processing = dbMan.getFilesNeedingProcessing(DedupMode::BALANCED);
    EXPECT_EQ(files_needing_processing.size(), 1);
    EXPECT_EQ(files_needing_processing[0].first, test_file);

    // Simulate processing by setting processing flag
    dbMan.setProcessingFlag(test_file, DedupMode::BALANCED);
    dbMan.waitForWrites();

    // Now should not need processing
    files_needing_processing = dbMan.getFilesNeedingProcessing(DedupMode::BALANCED);
    EXPECT_EQ(files_needing_processing.size(), 0);

    // Modify the file content (simulating file change)
    createTestFile(test_file, "different content");

    // Store the file again (should detect change and clear processing flags)
    dbMan.storeScannedFile(test_file);
    dbMan.waitForWrites();

    // Should need processing again (processing flags cleared)
    files_needing_processing = dbMan.getFilesNeedingProcessing(DedupMode::BALANCED);
    EXPECT_EQ(files_needing_processing.size(), 1);
    EXPECT_EQ(files_needing_processing[0].first, test_file);

    // Clean up test file
    fs::remove(test_file);
}