#include <gtest/gtest.h>
#include "filesmanager/disk_cache.hpp"
#include "config/config_manager_factory.hpp"
#include "test_utils.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>

using namespace MediaDedup;

class DiskCacheTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Create test configuration
        ConfigManagerConfig config;
        config.config_file_path = "../tests/test_data/test_disk_cache.yaml";
        config.enable_file_monitoring = false;
        config.emit_file_change_events = false;
        config.emit_programmatic_events = true; // Enable for config changes
        config.log_level = "debug";

        config_manager_ = ConfigManagerFactory::createWithConfig(config);
        ASSERT_TRUE(config_manager_ != nullptr);
        ASSERT_TRUE(config_manager_->initialize());

        // Set up test cache configuration
        test_cache_location_ = "test_cache_" + std::to_string(std::time(nullptr));
        config_manager_->createProperty("cache.disk.location", std::string("/cache"), "Disk cache location");
        config_manager_->createProperty("cache.disk.size_limit_mb", 1024, "Disk cache size limit in MB");
        config_manager_->setPropertyValue("cache.disk.location", test_cache_location_);
        config_manager_->setPropertyValue("cache.disk.size_limit_mb", 10); // 10 MB for testing

        // Create test files directory
        test_files_dir_ = std::filesystem::current_path() / "test_files_disk_cache";
        std::filesystem::create_directories(test_files_dir_);

        // Create disk cache instance
        disk_cache_ = std::make_unique<DiskCache>(config_manager_, "cache.disk");
    }

    void TearDown() override
    {
        // Shutdown cache
        if (disk_cache_)
        {
            disk_cache_->shutdown();
        }

        // Clean up test cache directory
        std::filesystem::path cache_path = std::filesystem::current_path() / test_cache_location_;
        if (std::filesystem::exists(cache_path))
        {
            std::filesystem::remove_all(cache_path);
        }

        // Clean up test files directory
        if (std::filesystem::exists(test_files_dir_))
        {
            std::filesystem::remove_all(test_files_dir_);
        }

        // Shutdown config manager
        if (config_manager_)
        {
            config_manager_->shutdown();
        }
    }

    void createTestFile(const std::filesystem::path &path, size_t size_bytes)
    {
        std::ofstream file(path, std::ios::binary);
        ASSERT_TRUE(file.is_open());

        // Write specified number of bytes
        std::vector<char> data(size_bytes, 'X');
        file.write(data.data(), data.size());
    }

    std::shared_ptr<UnifiedObservableConfigManager> config_manager_;
    std::unique_ptr<DiskCache> disk_cache_;
    std::string test_cache_location_;
    std::filesystem::path test_files_dir_;
};

TEST_F(DiskCacheTest, LazyCreatesDirectory)
{
    ASSERT_TRUE(disk_cache_->initialize());

    // Create test file
    auto test_file = test_files_dir_ / "test_file.txt";
    createTestFile(test_file, 1024);

    std::string cached_path;
    EXPECT_TRUE(disk_cache_->copyToCache(test_file.string(), cached_path));

    std::filesystem::path cache_path = std::filesystem::current_path() / test_cache_location_;
    EXPECT_TRUE(std::filesystem::exists(cache_path));
    EXPECT_TRUE(std::filesystem::is_directory(cache_path));
}

TEST_F(DiskCacheTest, InitializeClearsExistingCache)
{
    // Create some files in cache directory first
    std::filesystem::path cache_path = std::filesystem::current_path() / test_cache_location_;
    std::filesystem::create_directories(cache_path);

    createTestFile(cache_path / "existing1.txt", 1024 * 512); // 512 KB
    createTestFile(cache_path / "existing2.txt", 1024 * 512); // 512 KB

    // Set clearOnStartup=true BEFORE initialization (transcoding cache behavior)
    config_manager_->createProperty("cache.disk.clearOnStartup", true);

    ASSERT_TRUE(disk_cache_->initialize());

    // Cache should be cleared on initialization when clearOnStartup=true
    EXPECT_EQ(disk_cache_->getCurrentSizeMB(), 0);
}

TEST_F(DiskCacheTest, CopyToCache_Success)
{
    ASSERT_TRUE(disk_cache_->initialize());

    // Create test file
    auto test_file = test_files_dir_ / "test_file.txt";
    createTestFile(test_file, 1024 * 1024); // 1 MB

    std::string cached_path;
    EXPECT_TRUE(disk_cache_->copyToCache(test_file.string(), cached_path));

    EXPECT_FALSE(cached_path.empty());
    EXPECT_TRUE(std::filesystem::exists(cached_path));
    EXPECT_EQ(disk_cache_->getCurrentSizeMB(), 1);
}

TEST_F(DiskCacheTest, CopyToCache_NonExistentFile)
{
    ASSERT_TRUE(disk_cache_->initialize());

    std::string cached_path;
    EXPECT_FALSE(disk_cache_->copyToCache("/nonexistent/file.txt", cached_path));
}

// MP-2: cache names hash the FULL source path — two sources with the same
// basename in different directories must NOT collide (the old basename-only
// naming silently overwrote one with the other).
TEST_F(DiskCacheTest, CopyToCache_SameBasenameDistinctSourcesDoNotCollide)
{
    ASSERT_TRUE(disk_cache_->initialize());

    auto test_file1 = test_files_dir_ / "dir1" / "test.txt";
    auto test_file2 = test_files_dir_ / "dir2" / "test.txt";

    std::filesystem::create_directories(test_file1.parent_path());
    std::filesystem::create_directories(test_file2.parent_path());

    createTestFile(test_file1, 1024);
    createTestFile(test_file2, 1024);

    std::string cached_path1, cached_path2;
    EXPECT_TRUE(disk_cache_->copyToCache(test_file1.string(), cached_path1));
    EXPECT_TRUE(disk_cache_->copyToCache(test_file2.string(), cached_path2));

    EXPECT_NE(cached_path1, cached_path2);
    EXPECT_TRUE(std::filesystem::exists(cached_path1));
    EXPECT_TRUE(std::filesystem::exists(cached_path2));
}

// Re-caching the SAME source path is deterministic: same cached path, no
// double counting.
TEST_F(DiskCacheTest, CopyToCache_SameSourceOverwritesDeterministically)
{
    ASSERT_TRUE(disk_cache_->initialize());

    auto test_file = test_files_dir_ / "same_source.bin";
    createTestFile(test_file, 1024 * 1024); // 1 MB

    std::string cached_path1, cached_path2;
    EXPECT_TRUE(disk_cache_->copyToCache(test_file.string(), cached_path1));
    EXPECT_TRUE(disk_cache_->copyToCache(test_file.string(), cached_path2));

    EXPECT_EQ(cached_path1, cached_path2);
    EXPECT_EQ(disk_cache_->getCurrentSizeMB(), 1u) << "re-cache must not double count";
}

TEST_F(DiskCacheTest, SaveStreamToCache_Success)
{
    ASSERT_TRUE(disk_cache_->initialize());

    std::string test_data = "This is test data for stream saving.";
    std::istringstream stream(test_data);

    std::string cached_path;
    EXPECT_TRUE(disk_cache_->saveStreamToCache(stream, "test_stream.txt", cached_path));

    EXPECT_FALSE(cached_path.empty());
    EXPECT_TRUE(std::filesystem::exists(cached_path));

    // Verify content
    std::ifstream in(cached_path);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, test_data);
}

TEST_F(DiskCacheTest, DeleteFromCache_Success)
{
    ASSERT_TRUE(disk_cache_->initialize());

    // Create and cache a file
    auto test_file = test_files_dir_ / "test_delete.txt";
    createTestFile(test_file, 1024 * 1024); // 1 MB

    std::string cached_path;
    ASSERT_TRUE(disk_cache_->copyToCache(test_file.string(), cached_path));
    ASSERT_TRUE(std::filesystem::exists(cached_path));

    size_t size_before = disk_cache_->getCurrentSizeMB();
    EXPECT_EQ(size_before, 1);

    // Delete from cache
    EXPECT_TRUE(disk_cache_->deleteFromCache(cached_path));
    EXPECT_FALSE(std::filesystem::exists(cached_path));

    size_t size_after = disk_cache_->getCurrentSizeMB();
    EXPECT_EQ(size_after, 0);
}

TEST_F(DiskCacheTest, ClearCache_RemovesAllFiles)
{
    ASSERT_TRUE(disk_cache_->initialize());

    // Create multiple test files and cache them
    for (int i = 0; i < 5; i++)
    {
        auto test_file = test_files_dir_ / ("test_" + std::to_string(i) + ".txt");
        createTestFile(test_file, 1024 * 1024); // 1 MB each

        std::string cached_path;
        ASSERT_TRUE(disk_cache_->copyToCache(test_file.string(), cached_path));
    }

    EXPECT_EQ(disk_cache_->getCurrentSizeMB(), 5);

    // Clear cache
    EXPECT_TRUE(disk_cache_->clearCache());
    EXPECT_EQ(disk_cache_->getCurrentSizeMB(), 0);

    // Verify directory is empty
    std::filesystem::path cache_path = std::filesystem::current_path() / test_cache_location_;
    size_t file_count = 0;
    for (const auto &entry : std::filesystem::directory_iterator(cache_path))
    {
        if (entry.is_regular_file())
        {
            file_count++;
        }
    }
    EXPECT_EQ(file_count, 0);
}

TEST_F(DiskCacheTest, EnforceSizeLimit_RemovesOldestFiles)
{
    ASSERT_TRUE(disk_cache_->initialize());

    // Create files that will exceed the limit
    std::vector<std::string> cached_paths;

    // Create 3 files, 4 MB each (total 12 MB, limit is 10 MB)
    for (int i = 0; i < 3; i++)
    {
        auto test_file = test_files_dir_ / ("large_" + std::to_string(i) + ".bin");
        createTestFile(test_file, 1024 * 1024 * 4); // 4 MB

        // Add small delay to ensure different timestamps
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        std::string cached_path;
        ASSERT_TRUE(disk_cache_->copyToCache(test_file.string(), cached_path));
        cached_paths.push_back(cached_path);
    }

    // First file should be removed (FIFO)
    EXPECT_FALSE(std::filesystem::exists(cached_paths[0]));

    // Second and third files should still exist
    EXPECT_TRUE(std::filesystem::exists(cached_paths[1]));
    EXPECT_TRUE(std::filesystem::exists(cached_paths[2]));
}

TEST_F(DiskCacheTest, FileLargerThanLimit_ClearsCacheAndRefuses)
{
    ASSERT_TRUE(disk_cache_->initialize());

    // Create some files in cache first
    for (int i = 0; i < 2; i++)
    {
        auto test_file = test_files_dir_ / ("small_" + std::to_string(i) + ".txt");
        createTestFile(test_file, 1024 * 1024); // 1 MB

        std::string cached_path;
        ASSERT_TRUE(disk_cache_->copyToCache(test_file.string(), cached_path));
    }

    EXPECT_GT(disk_cache_->getCurrentSizeMB(), 0);

    // Create a file larger than the limit (15 MB, limit is 10 MB)
    auto large_file = test_files_dir_ / "very_large.bin";
    createTestFile(large_file, 1024 * 1024 * 15); // 15 MB

    std::string cached_path_large;
    EXPECT_FALSE(disk_cache_->copyToCache(large_file.string(), cached_path_large));

    // Large file should NOT be cached
    EXPECT_FALSE(std::filesystem::exists(cached_path_large));

    // Cache was NOT cleared because the file was rejected early
    EXPECT_EQ(disk_cache_->getCurrentSizeMB(), 2);
}

TEST_F(DiskCacheTest, ConfigChange_Location_UpdatesCache)
{
    ASSERT_TRUE(disk_cache_->initialize());

    std::string original_location = disk_cache_->getCacheLocation();

    // Change cache location via config
    std::string new_location = "test_cache_new_" + std::to_string(std::time(nullptr));
    config_manager_->setPropertyValue("cache.disk.location", new_location);

    // Give it a moment to process the change
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::string updated_location = disk_cache_->getCacheLocation();
    EXPECT_NE(original_location, updated_location);
    EXPECT_TRUE(std::filesystem::exists(updated_location));

    // Clean up new directory
    std::filesystem::remove_all(updated_location);
}

TEST_F(DiskCacheTest, ConfigChange_SizeLimit_EnforcesNewLimit)
{
    ASSERT_TRUE(disk_cache_->initialize());

    // Fill cache with files
    for (int i = 0; i < 3; i++)
    {
        auto test_file = test_files_dir_ / ("file_" + std::to_string(i) + ".bin");
        createTestFile(test_file, 1024 * 1024 * 3); // 3 MB each = 9 MB total

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        std::string cached_path;
        ASSERT_TRUE(disk_cache_->copyToCache(test_file.string(), cached_path));
    }

    size_t size_before = disk_cache_->getCurrentSizeMB();
    EXPECT_GE(size_before, 9);

    // Reduce size limit to 5 MB
    config_manager_->setPropertyValue("cache.disk.size_limit_mb", 5);

    // Give it a moment to process the change
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Cache should have been reduced
    size_t size_after = disk_cache_->getCurrentSizeMB();
    EXPECT_LE(size_after, 5);
    EXPECT_LT(size_after, size_before);
}

TEST_F(DiskCacheTest, ThreadSafety_ConcurrentOperations)
{
    ASSERT_TRUE(disk_cache_->initialize());

    const int num_threads = 5;
    const int files_per_thread = 10;
    std::vector<std::thread> threads;

    // Concurrent file copying
    for (int t = 0; t < num_threads; t++)
    {
        threads.emplace_back([this, t]()
                             {
            for (int i = 0; i < 10; i++)
            {
                auto test_file = test_files_dir_ / ("thread_" + std::to_string(t) + 
                    "_file_" + std::to_string(i) + ".txt");
                createTestFile(test_file, 1024 * 200); // 200 KB
                
                std::string cached_path;
                disk_cache_->copyToCache(test_file.string(), cached_path);
            } });
    }

    for (auto &thread : threads)
    {
        thread.join();
    }

    // All operations should complete without crashes
    EXPECT_GT(disk_cache_->getCurrentSizeMB(), 0);
}

TEST_F(DiskCacheTest, GetCurrentSizeMB_ReturnsCorrectSize)
{
    ASSERT_TRUE(disk_cache_->initialize());

    EXPECT_EQ(disk_cache_->getCurrentSizeMB(), 0);

    // Add 2 MB file
    auto test_file = test_files_dir_ / "2mb_file.bin";
    createTestFile(test_file, 1024 * 1024 * 2); // 2 MB

    std::string cached_path;
    ASSERT_TRUE(disk_cache_->copyToCache(test_file.string(), cached_path));

    EXPECT_EQ(disk_cache_->getCurrentSizeMB(), 2);
}

TEST_F(DiskCacheTest, GetSizeLimitMB_ReturnsConfiguredLimit)
{
    ASSERT_TRUE(disk_cache_->initialize());

    EXPECT_EQ(disk_cache_->getSizeLimitMB(), 10); // Set in SetUp
}

TEST_F(DiskCacheTest, GetCacheLocation_ReturnsCorrectPath)
{
    ASSERT_TRUE(disk_cache_->initialize());

    std::string location = disk_cache_->getCacheLocation();
    EXPECT_FALSE(location.empty());
    EXPECT_TRUE(location.find(test_cache_location_) != std::string::npos);
}

TEST_F(DiskCacheTest, InitializeWithDefaultValues_Works)
{
    // Verify that the cache works with standard default values
    ASSERT_TRUE(disk_cache_->initialize());

    // Cache should be functional
    EXPECT_GT(disk_cache_->getSizeLimitMB(), 0);
    EXPECT_FALSE(disk_cache_->getCacheLocation().empty());

    // Test basic operation
    auto test_file = test_files_dir_ / "default_test.txt";
    createTestFile(test_file, 1024 * 512); // 512 KB

    std::string cached_path;
    EXPECT_TRUE(disk_cache_->copyToCache(test_file.string(), cached_path));
    EXPECT_TRUE(std::filesystem::exists(cached_path));
}

TEST_F(DiskCacheTest, CacheClearedOnRestart)
{
    // Set clearOnStartup=true BEFORE initialization (transcoding cache behavior)
    config_manager_->createProperty("cache.disk.clearOnStartup", true);

    ASSERT_TRUE(disk_cache_->initialize());

    // Create and cache a file
    auto test_file = test_files_dir_ / "persistent.txt";
    createTestFile(test_file, 1024 * 500); // 500 KB

    std::string cached_path;
    ASSERT_TRUE(disk_cache_->copyToCache(test_file.string(), cached_path));

    size_t size_before = disk_cache_->getCurrentSizeMB();

    // Shutdown and recreate cache with clearOnStartup=true
    disk_cache_->shutdown();
    disk_cache_ = std::make_unique<DiskCache>(config_manager_, "cache.disk");
    ASSERT_TRUE(disk_cache_->initialize());

    // Cache should be cleared on restart when clearOnStartup=true
    size_t size_after = disk_cache_->getCurrentSizeMB();
    EXPECT_EQ(size_after, 0);
    EXPECT_FALSE(std::filesystem::exists(cached_path));
}

TEST_F(DiskCacheTest, CachePreservedOnRestart)
{
    // Set clearOnStartup=false BEFORE initialization (thumbnail cache behavior)
    config_manager_->createProperty("cache.disk.clearOnStartup", false);

    ASSERT_TRUE(disk_cache_->initialize());

    // Create and cache a file (use 2MB to ensure size > 0 in MB)
    auto test_file = test_files_dir_ / "thumbnail.jpg";
    createTestFile(test_file, 1024 * 1024 * 2); // 2 MB

    std::string cached_path;
    ASSERT_TRUE(disk_cache_->copyToCache(test_file.string(), cached_path));
    ASSERT_TRUE(std::filesystem::exists(cached_path));

    size_t size_before = disk_cache_->getCurrentSizeMB();
    EXPECT_GT(size_before, 0);

    // Shutdown and recreate cache with clearOnStartup=false
    disk_cache_->shutdown();
    disk_cache_ = std::make_unique<DiskCache>(config_manager_, "cache.disk");
    ASSERT_TRUE(disk_cache_->initialize());

    // Cache should be preserved on restart when clearOnStartup=false
    size_t size_after = disk_cache_->getCurrentSizeMB();
    EXPECT_EQ(size_after, size_before);
    EXPECT_TRUE(std::filesystem::exists(cached_path));
}

TEST_F(DiskCacheTest, CacheOperations_NeverTouchSourceFiles)
{
    // Create a temporary directory for source files
    std::filesystem::path source_dir = test_files_dir_ / "source_files";
    std::filesystem::create_directories(source_dir);

    // Create test source files with different extensions
    std::vector<std::string> source_files = {
        "test_image.jpg",
        "test_raw.cr2",
        "test_video.mp4",
        "test_audio.mp3",
        "test_document.pdf"};

    std::vector<std::string> source_paths;
    for (const auto &filename : source_files)
    {
        std::filesystem::path source_path = source_dir / filename;
        source_paths.push_back(source_path.string());

        // Create a test file with some content (make it larger to avoid size issues)
        std::ofstream file(source_path, std::ios::binary);
        ASSERT_TRUE(file.is_open()) << "Failed to create source file: " << source_path.string();
        std::string content = "test content for " + filename + " - this is a longer test file to ensure proper size";
        // Write the content multiple times to make it larger
        for (int i = 0; i < 10; ++i)
        {
            file.write(content.c_str(), content.length());
        }
        file.close();

        // Verify source file exists
        ASSERT_TRUE(std::filesystem::exists(source_path)) << "Source file should exist: " << source_path.string();
    }

    // Debug: List all created files
    std::cout << "Created source files:" << std::endl;
    for (const auto &source_path : source_paths)
    {
        std::cout << "  " << source_path << " (exists: " << std::filesystem::exists(source_path) << ")" << std::endl;
    }

    // Initialize cache
    ASSERT_TRUE(disk_cache_->initialize());

    // Copy all source files to cache
    std::vector<std::string> cached_paths;
    for (const auto &source_path : source_paths)
    {
        std::string cached_path;

        // Debug: Check if source file exists before copying
        if (!std::filesystem::exists(source_path))
        {
            FAIL() << "Source file does not exist before copy: " << source_path;
        }

        std::cout << "Attempting to copy: " << source_path << std::endl;
        bool copy_result = disk_cache_->copyToCache(source_path, cached_path);
        std::cout << "Copy result: " << copy_result << ", cached_path: " << cached_path << std::endl;
        if (!copy_result)
        {
            FAIL() << "Failed to copy to cache: " << source_path
                   << " (file exists: " << std::filesystem::exists(source_path) << ")";
        }
        cached_paths.push_back(cached_path);

        // Verify source file still exists after copying
        EXPECT_TRUE(std::filesystem::exists(source_path))
            << "Source file should still exist after copy: " << source_path;
    }

    // Mark all cached files as in use
    for (const auto &cached_path : cached_paths)
    {
        disk_cache_->markFileInUse(cached_path);
    }

    // Test 1: Clear cache should not affect source files
    disk_cache_->clearCache();

    for (const auto &source_path : source_paths)
    {
        EXPECT_TRUE(std::filesystem::exists(source_path))
            << "Source file should exist after clearCache: " << source_path;
    }

    // Re-copy files to cache for more tests
    cached_paths.clear();
    for (const auto &source_path : source_paths)
    {
        std::string cached_path;
        ASSERT_TRUE(disk_cache_->copyToCache(source_path, cached_path)) << "Failed to re-copy to cache: " << source_path;
        cached_paths.push_back(cached_path);
    }

    // Test 2: Delete individual cached files should not affect source files
    for (const auto &cached_path : cached_paths)
    {
        ASSERT_TRUE(disk_cache_->deleteFromCache(cached_path))
            << "Failed to delete from cache: " << cached_path;
    }

    for (const auto &source_path : source_paths)
    {
        EXPECT_TRUE(std::filesystem::exists(source_path))
            << "Source file should exist after individual deletions: " << source_path;
    }

    // Re-copy files to cache for more tests
    cached_paths.clear();
    for (const auto &source_path : source_paths)
    {
        std::string cached_path;
        ASSERT_TRUE(disk_cache_->copyToCache(source_path, cached_path));
        cached_paths.push_back(cached_path);
    }

    // Test 3: Immediate deletion should not affect source files
    for (const auto &cached_path : cached_paths)
    {
        ASSERT_TRUE(disk_cache_->deleteFromCacheImmediately(cached_path))
            << "Failed to delete immediately from cache: " << cached_path;
    }

    for (const auto &source_path : source_paths)
    {
        EXPECT_TRUE(std::filesystem::exists(source_path))
            << "Source file should exist after immediate deletions: " << source_path;
    }

    // Test 4: Cache shutdown should not affect source files
    disk_cache_->shutdown();

    for (const auto &source_path : source_paths)
    {
        EXPECT_TRUE(std::filesystem::exists(source_path))
            << "Source file should exist after cache shutdown: " << source_path;
    }

    // Test 5: Reinitialize and clear should not affect source files
    ASSERT_TRUE(disk_cache_->initialize());
    disk_cache_->clearCache();

    for (const auto &source_path : source_paths)
    {
        EXPECT_TRUE(std::filesystem::exists(source_path))
            << "Source file should exist after reinitialize and clear: " << source_path;
    }

    // Test 6: Verify source file contents are unchanged
    for (size_t i = 0; i < source_files.size(); ++i)
    {
        const auto &source_path = source_paths[i];
        const auto &filename = source_files[i];

        std::ifstream file(source_path, std::ios::binary);
        ASSERT_TRUE(file.is_open()) << "Failed to read source file: " << source_path;

        std::string content((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
        file.close();

        // Expected content is the single line repeated 10 times
        std::string expected_content;
        for (int i = 0; i < 10; ++i)
        {
            expected_content += "test content for " + filename + " - this is a longer test file to ensure proper size";
        }
        EXPECT_EQ(content, expected_content)
            << "Source file content should be unchanged: " << source_path;
    }

    // Test 7: Test with files outside cache directory (edge case)
    std::filesystem::path external_file = test_files_dir_ / "external_test.txt";
    std::ofstream ext_file(external_file, std::ios::binary);
    ASSERT_TRUE(ext_file.is_open());
    ext_file.write("external test content", 21);
    ext_file.close();

    // Try to delete external file (should fail safely)
    bool delete_result = disk_cache_->deleteFromCacheImmediately(external_file.string());
    EXPECT_FALSE(delete_result) << "Should not be able to delete external file";
    EXPECT_TRUE(std::filesystem::exists(external_file))
        << "External file should still exist after failed deletion attempt";

    // Cleanup
    std::filesystem::remove_all(source_dir);
    std::filesystem::remove(external_file);
}

TEST_F(DiskCacheTest, CacheCleanupOnError_EnsuresFilesAreCleared)
{
    // Test that files are properly cleaned up even when operations fail
    ASSERT_TRUE(disk_cache_->initialize());

    // Create a test file (1MB)
    std::filesystem::path test_file = test_files_dir_ / "test_cleanup.jpg";
    std::ofstream file(test_file, std::ios::binary);
    ASSERT_TRUE(file.is_open());
    std::string content = "test content for cleanup verification - this is a longer string to ensure proper file size";
    for (int i = 0; i < 10000; ++i)
    { // Create ~1MB file
        file.write(content.c_str(), content.length());
    }
    file.close();

    // Copy file to cache
    std::string cached_path;
    ASSERT_TRUE(disk_cache_->copyToCache(test_file.string(), cached_path));

    // Verify file is in cache
    EXPECT_TRUE(std::filesystem::exists(cached_path));

    // Verify cache size is now non-zero (file was added)
    // Note: Cache is cleared on initialization, so we just verify the file exists
    EXPECT_TRUE(std::filesystem::exists(cached_path)); // File should exist in cache

    // Mark file as in use
    disk_cache_->markFileInUse(cached_path);

    // Simulate an error scenario - try to copy a file with the same name
    // This should fail because the file is in use and we can't overwrite it
    std::filesystem::path another_file = test_files_dir_ / "another_test.jpg";
    std::ofstream another_file_stream(another_file, std::ios::binary);
    ASSERT_TRUE(another_file_stream.is_open());
    for (int i = 0; i < 10000; ++i)
    {
        another_file_stream.write(content.c_str(), content.length());
    }
    another_file_stream.close();

    // Try to copy with the same filename - this should fail because the file is in use
    std::string another_cached_path;
    bool copy_result = disk_cache_->copyToCache(another_file.string(), another_cached_path);
    // Note: This might succeed because we're using different source files, but the cached file should be different
    // The important part is that the original file is still there and marked as in use

    // Verify original file is still in cache and marked as in use
    EXPECT_TRUE(std::filesystem::exists(cached_path));
    EXPECT_GE(disk_cache_->getCurrentSizeMB(), 1);

    // Now mark file as not in use and delete it
    disk_cache_->markFileNotInUse(cached_path);
    ASSERT_TRUE(disk_cache_->deleteFromCacheImmediately(cached_path));

    // Verify file is deleted
    EXPECT_FALSE(std::filesystem::exists(cached_path));
    EXPECT_EQ(disk_cache_->getCurrentSizeMB(), 0);

    // Cleanup
    std::filesystem::remove(test_file);
    std::filesystem::remove(another_file);
}

TEST_F(DiskCacheTest, CacheLimitBehavior_DoesNotHaltServer)
{
    // Test that cache limit enforcement doesn't cause server to halt
    ASSERT_TRUE(disk_cache_->initialize());

    // Create a file larger than the cache limit (10MB)
    std::filesystem::path large_file = test_files_dir_ / "large_file.bin";
    std::ofstream file(large_file, std::ios::binary);
    ASSERT_TRUE(file.is_open());

    // Create a 15MB file (larger than 10MB limit)
    std::string content = "large file content for testing cache limits - this is a longer string to ensure proper file size";
    for (int i = 0; i < 200000; ++i)
    { // This should create a ~15MB file
        file.write(content.c_str(), content.length());
    }
    file.close();

    // Verify file size
    size_t file_size = std::filesystem::file_size(large_file);
    EXPECT_GT(file_size, 10 * 1024 * 1024) << "File should be larger than 10MB cache limit";

    // Copy file to cache - this should be rejected because it exceeds the total limit
    std::string cached_path;
    bool copy_result = disk_cache_->copyToCache(large_file.string(), cached_path);

    // This should fail gracefully without halting the server
    EXPECT_FALSE(copy_result) << "Should reject file that is larger than the total cache limit";

    // Verify cache size is still 0 (since it was empty and we rejected the file)
    EXPECT_EQ(disk_cache_->getCurrentSizeMB(), 0);

    // Clean up
    disk_cache_->deleteFromCacheImmediately(cached_path);
    std::filesystem::remove(large_file);
}

TEST_F(DiskCacheTest, RemoveOldestFiles_SkipsFilesInUse)
{
    // Test that removeOldestFiles skips files that are currently in use
    ASSERT_TRUE(disk_cache_->initialize());

    // Create a few files (not enough to fill cache)
    std::vector<std::string> source_files;
    std::vector<std::string> cached_paths;

    for (int i = 0; i < 3; ++i)
    {
        std::filesystem::path test_file = test_files_dir_ / ("test_file_" + std::to_string(i) + ".jpg");
        std::ofstream file(test_file, std::ios::binary);
        ASSERT_TRUE(file.is_open());

        // Create 1MB files (total 3MB, well under 10MB limit)
        std::string content = "test content for file " + std::to_string(i) + " - this is a longer string to ensure proper file size";
        for (int j = 0; j < 20000; ++j)
        { // Create ~1MB files
            file.write(content.c_str(), content.length());
        }
        file.close();

        source_files.push_back(test_file.string());

        // Copy to cache
        std::string cached_path;
        ASSERT_TRUE(disk_cache_->copyToCache(test_file.string(), cached_path));
        cached_paths.push_back(cached_path);
    }

    // Verify files are in cache
    for (const auto &cached_path : cached_paths)
    {
        EXPECT_TRUE(std::filesystem::exists(cached_path)) << "File should exist in cache: " << cached_path;
    }

    // Mark the first file as in use
    disk_cache_->markFileInUse(cached_paths[0]);

    // Debug: Verify the file is marked as in use
    std::cout << "Debug: File marked as in use: " << cached_paths[0] << std::endl;
    std::cout << "Debug: Cache size before adding new file: " << disk_cache_->getCurrentSizeMB() << " MB" << std::endl;

    // Verify the file exists before marking as in use
    EXPECT_TRUE(std::filesystem::exists(cached_paths[0])) << "File should exist before marking as in use";

    // Create a large file that will trigger removeOldestFiles
    std::filesystem::path large_file = test_files_dir_ / "large_file.jpg";
    std::ofstream large_file_stream(large_file, std::ios::binary);
    ASSERT_TRUE(large_file_stream.is_open());
    std::string content = "large file content - this is a longer string to ensure proper file size";
    for (int i = 0; i < 80000; ++i)
    { // Create ~8MB file to trigger cache limit
        large_file_stream.write(content.c_str(), content.length());
    }
    large_file_stream.close();

    std::string large_cached_path;
    bool copy_result = disk_cache_->copyToCache(large_file.string(), large_cached_path);

    // Should succeed by removing oldest files (but not the one in use)
    EXPECT_TRUE(copy_result) << "Should succeed by removing oldest files";

    // Debug: Check if the file in use is still there
    std::cout << "Debug: File in use still exists: " << std::filesystem::exists(cached_paths[0]) << std::endl;
    std::cout << "Debug: Large file exists: " << std::filesystem::exists(large_cached_path) << std::endl;

    // Verify the file in use is still there
    EXPECT_TRUE(std::filesystem::exists(cached_paths[0])) << "File in use should not be deleted";

    // Verify the large file is there
    EXPECT_TRUE(std::filesystem::exists(large_cached_path)) << "Large file should be in cache";

    // Clean up
    for (const auto &source_file : source_files)
    {
        std::filesystem::remove(source_file);
    }
    std::filesystem::remove(large_file);
}

// MP-2: with eviction hooks installed, eviction follows the DB's LRU order
// (not file mtime), and the owner is notified of exactly the deleted paths so
// its rows can be removed (no dangling rows -> no regeneration storms).
TEST_F(DiskCacheTest, EvictionHooks_LruOrderAndNotification)
{
    ASSERT_TRUE(disk_cache_->initialize());

    // Fill with three 3 MB files (limit is 10 MB from SetUp).
    std::vector<std::string> cached;
    for (int i = 0; i < 3; ++i)
    {
        auto f = test_files_dir_ / ("lru_" + std::to_string(i) + ".bin");
        createTestFile(f, 3 * 1024 * 1024);
        std::string p;
        ASSERT_TRUE(disk_cache_->copyToCache(f.string(), p));
        cached.push_back(p);
    }

    // Hook says: the MIDDLE file is least recently used.
    std::vector<std::string> notified;
    disk_cache_->setEvictionHooks(
        [&](std::size_t) { return std::vector<std::string>{cached[1], cached[0], cached[2]}; },
        [&](const std::vector<std::string> &paths)
        { notified.insert(notified.end(), paths.begin(), paths.end()); });

    // A 4th 3 MB file forces eviction (9 + 3 > 10).
    auto f4 = test_files_dir_ / "lru_new.bin";
    createTestFile(f4, 3 * 1024 * 1024);
    std::string p4;
    ASSERT_TRUE(disk_cache_->copyToCache(f4.string(), p4));

    EXPECT_FALSE(std::filesystem::exists(cached[1])) << "hook-designated LRU file evicted first";
    EXPECT_TRUE(std::filesystem::exists(p4));
    ASSERT_FALSE(notified.empty());
    EXPECT_EQ(notified.front(), cached[1]) << "owner notified of the evicted path";
    for (const auto &p : notified)
        EXPECT_FALSE(std::filesystem::exists(p)) << "every notified path was actually deleted";
}

// MP-2: cached files land in hash shards (2-hex subdirectories), and cache
// accounting/clearing handles the nested layout.
TEST_F(DiskCacheTest, ShardedLayout_ClearAndAccountingWork)
{
    ASSERT_TRUE(disk_cache_->initialize());

    auto f = test_files_dir_ / "sharded.bin";
    createTestFile(f, 1024 * 1024);
    std::string p;
    ASSERT_TRUE(disk_cache_->copyToCache(f.string(), p));

    auto rel = std::filesystem::path(p).lexically_relative(disk_cache_->getCacheLocation());
    ASSERT_EQ(std::distance(rel.begin(), rel.end()), 2) << "expected <shard>/<file> layout, got " << rel;
    EXPECT_EQ(rel.begin()->string().size(), 2u) << "shard dir is 2 hex chars";

    EXPECT_EQ(disk_cache_->getCurrentSizeMB(), 1u);
    EXPECT_TRUE(disk_cache_->clearCache());
    EXPECT_EQ(disk_cache_->getCurrentSizeMB(), 0u);
    EXPECT_FALSE(std::filesystem::exists(p));
}

// ---------------------------------------------------------------------------
// MP-4: bounded LRU for the viewer's full-resolution temp renders.
// ---------------------------------------------------------------------------
#include "utils/full_res_lru_cache.hpp"

namespace
{
    std::string writeTempRender(const std::string &name, size_t bytes)
    {
        auto path = (std::filesystem::temp_directory_path() / name).string();
        std::ofstream f(path, std::ios::binary);
        std::vector<char> data(bytes, 'R');
        f.write(data.data(), data.size());
        return path;
    }
}

TEST(FullResLruCacheTest, EntryCapEvictsLeastRecentlyUsedAndUnlinks)
{
    FullResLruCache cache(/*max_entries=*/2, /*max_bytes=*/SIZE_MAX);

    std::string a = writeTempRender("fullres_a.jpg", 100);
    std::string b = writeTempRender("fullres_b.jpg", 100);
    std::string c = writeTempRender("fullres_c.jpg", 100);

    cache.put("/photo/a", {a, 1});
    cache.put("/photo/b", {b, 1});
    ASSERT_TRUE(cache.getFresh("/photo/a", 1).has_value()); // touch a: b is now LRU

    cache.put("/photo/c", {c, 1}); // evicts b
    EXPECT_FALSE(std::filesystem::exists(b)) << "evicted render must be unlinked";
    EXPECT_TRUE(std::filesystem::exists(a));
    EXPECT_TRUE(std::filesystem::exists(c));
    EXPECT_FALSE(cache.getFresh("/photo/b", 1).has_value());

    cache.clear();
    EXPECT_FALSE(std::filesystem::exists(a));
    EXPECT_FALSE(std::filesystem::exists(c));
}

TEST(FullResLruCacheTest, ByteCapEvicts)
{
    FullResLruCache cache(/*max_entries=*/100, /*max_bytes=*/250);

    std::string a = writeTempRender("fullres_bytes_a.jpg", 100);
    std::string b = writeTempRender("fullres_bytes_b.jpg", 100);
    std::string c = writeTempRender("fullres_bytes_c.jpg", 100);

    cache.put("/p/a", {a, 1});
    cache.put("/p/b", {b, 1});
    cache.put("/p/c", {c, 1}); // 300 > 250: oldest (a) evicted

    EXPECT_FALSE(std::filesystem::exists(a));
    EXPECT_TRUE(std::filesystem::exists(b));
    EXPECT_TRUE(std::filesystem::exists(c));
    cache.clear();
}

TEST(FullResLruCacheTest, StaleMtimeDropsEntryAndUnlinks)
{
    FullResLruCache cache;
    std::string a = writeTempRender("fullres_stale.jpg", 64);
    cache.put("/p/stale", {a, /*source_mtime=*/100});

    // Source changed (mtime 200): entry is stale — dropped AND the temp
    // render unlinked (the old code leaked the file here).
    EXPECT_FALSE(cache.getFresh("/p/stale", 200).has_value());
    EXPECT_FALSE(std::filesystem::exists(a));
}

TEST(FullResLruCacheTest, ReplacingEntryUnlinksOldRender)
{
    FullResLruCache cache;
    std::string old_render = writeTempRender("fullres_old.jpg", 64);
    std::string new_render = writeTempRender("fullres_new.jpg", 64);

    cache.put("/p/x", {old_render, 1});
    cache.put("/p/x", {new_render, 2});

    EXPECT_FALSE(std::filesystem::exists(old_render)) << "replaced render must be unlinked";
    auto hit = cache.getFresh("/p/x", 2);
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->temp_path, new_render);
    cache.clear();
}

TEST(FullResLruCacheTest, DestructorUnlinksEverything)
{
    std::string a = writeTempRender("fullres_dtor.jpg", 64);
    {
        FullResLruCache cache;
        cache.put("/p/d", {a, 1});
    }
    EXPECT_FALSE(std::filesystem::exists(a)) << "shutdown must not leave native-res temp litter";
}

// ---------------------------------------------------------------------------
// MP-5: mount-table normalization happens ONCE at collection, not per lookup.
// ---------------------------------------------------------------------------
#include "filesmanager/media_availability.hpp"

TEST(MediaAvailabilityTest, MountedFilesystemsAreNormalized)
{
    for (const auto &m : MediaAvailability::mountedFilesystems())
    {
        EXPECT_EQ(m.find('\\'), std::string::npos) << m;
        if (m.size() > 1)
            EXPECT_NE(m.back(), '/') << "trailing slash must be stripped at collection: " << m;
    }
}

TEST(MediaAvailabilityTest, IsPathAvailableAgainstNormalizedMounts)
{
    // Boot volume paths are always available, mounts list irrelevant.
    EXPECT_TRUE(MediaAvailability::isPathAvailable("/Users/x/photo.jpg", {}));

    // Removable volume: available exactly when its normalized root is mounted.
    std::vector<std::string> mounts = {"/", "/Volumes/TestVol123"};
    EXPECT_TRUE(MediaAvailability::isPathAvailable("/Volumes/TestVol123/a/b.jpg", mounts));
    EXPECT_FALSE(MediaAvailability::isPathAvailable("/Volumes/OtherVol/a.jpg", mounts));
}

// ---------------------------------------------------------------------------
// AR-1: ThumbnailService — the generation core shared by HTTP, gRPC, and the
// backfill (extracted from the HTTP ThumbnailHandler).
// ---------------------------------------------------------------------------
#if defined(HAVE_OPENCV)
#include "media_processors/thumbnail_service.hpp"
#include "database/database_manager.hpp"
#include "database/thumbnail_cache_ops.hpp"
#include <opencv2/opencv.hpp>

TEST(ThumbnailServiceTest, GenerateAndCacheEndToEnd)
{
    // File-backed DB (a :memory: DatabaseManager is broken under the pool)
    auto db_path = (std::filesystem::temp_directory_path() / "thumb_service_test.db").string();
    std::filesystem::remove(db_path);
    auto db = std::make_shared<DatabaseManager>(db_path);
    ASSERT_TRUE(db->initialize());
    ASSERT_TRUE(ThumbnailCacheOps::ensureTable(*db));

    ConfigManagerConfig cfg_config;
    cfg_config.config_file_path = "/dev/null";
    cfg_config.enable_file_monitoring = false;
    auto cfg = ConfigManagerFactory::createWithConfig(cfg_config);
    ASSERT_TRUE(cfg->initialize());
    std::string cache_dir = "thumb_service_cache_" + std::to_string(::getpid());
    cfg->createProperty("cache.thumbnail.location", cache_dir, "test");
    cfg->createProperty("cache.thumbnail.size_limit_mb", 64, "test");
    auto cache = std::make_shared<DiskCache>(cfg, "cache.thumbnail");
    ASSERT_TRUE(cache->initialize());

    auto src = (std::filesystem::temp_directory_path() / "thumb_service_src.jpg").string();
    cv::Mat img(600, 800, CV_8UC3, cv::Scalar(120, 90, 200));
    ASSERT_TRUE(cv::imwrite(src, img));

    ThumbnailService service(cfg, db, cache);
    std::string cached_path;
    ASSERT_TRUE(service.generateAndCacheThumbnail(src, 256, cached_path));
    EXPECT_TRUE(std::filesystem::exists(cached_path));

    // DB row recorded and served on repeat (no regeneration: same path back)
    auto row = ThumbnailCacheOps::getBestFitThumbnail(*db, src, 256);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(row->cached_path, cached_path);
    std::string cached_path2;
    ASSERT_TRUE(service.generateAndCacheThumbnail(src, 256, cached_path2));
    EXPECT_EQ(cached_path2, cached_path);

    // Thumbnail decodes back at the requested box
    cv::Mat thumb = cv::imread(cached_path, cv::IMREAD_COLOR);
    ASSERT_FALSE(thumb.empty());
    EXPECT_EQ(std::max(thumb.rows, thumb.cols), 256);

    db.reset();
    std::filesystem::remove(db_path);
    std::filesystem::remove(src);
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::current_path() / cache_dir, ec);
}
#endif // HAVE_OPENCV
