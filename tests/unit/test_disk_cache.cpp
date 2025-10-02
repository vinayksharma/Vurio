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
        disk_cache_ = std::make_unique<DiskCache>(config_manager_);
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

TEST_F(DiskCacheTest, InitializeCreatesDirectory)
{
    ASSERT_TRUE(disk_cache_->initialize());

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

    ASSERT_TRUE(disk_cache_->initialize());

    // Cache should be cleared on initialization, so size should be 0
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

TEST_F(DiskCacheTest, CopyToCache_OverwritesSameName)
{
    ASSERT_TRUE(disk_cache_->initialize());

    // Create two test files with same name in different directories
    auto test_file1 = test_files_dir_ / "dir1" / "test.txt";
    auto test_file2 = test_files_dir_ / "dir2" / "test.txt";

    std::filesystem::create_directories(test_file1.parent_path());
    std::filesystem::create_directories(test_file2.parent_path());

    createTestFile(test_file1, 1024);
    createTestFile(test_file2, 1024);

    std::string cached_path1, cached_path2;
    EXPECT_TRUE(disk_cache_->copyToCache(test_file1.string(), cached_path1));
    EXPECT_TRUE(disk_cache_->copyToCache(test_file2.string(), cached_path2));

    // Paths should be the same since we removed hash naming and files overwrite
    EXPECT_EQ(cached_path1, cached_path2);
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

TEST_F(DiskCacheTest, FileLargerThanLimit_ClearsCacheAndAllows)
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

    std::string cached_path;
    EXPECT_TRUE(disk_cache_->copyToCache(large_file.string(), cached_path));

    // Large file should be cached
    EXPECT_TRUE(std::filesystem::exists(cached_path));

    // Cache should now exceed limit (warning logged)
    EXPECT_GT(disk_cache_->getCurrentSizeMB(), 10);
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
    ASSERT_TRUE(disk_cache_->initialize());

    // Create and cache a file
    auto test_file = test_files_dir_ / "persistent.txt";
    createTestFile(test_file, 1024 * 500); // 500 KB

    std::string cached_path;
    ASSERT_TRUE(disk_cache_->copyToCache(test_file.string(), cached_path));

    size_t size_before = disk_cache_->getCurrentSizeMB();

    // Shutdown and recreate cache
    disk_cache_->shutdown();
    disk_cache_ = std::make_unique<DiskCache>(config_manager_);
    ASSERT_TRUE(disk_cache_->initialize());

    // Cache should be cleared on restart, so size should be 0
    size_t size_after = disk_cache_->getCurrentSizeMB();
    EXPECT_EQ(size_after, 0);
    EXPECT_FALSE(std::filesystem::exists(cached_path));
}
