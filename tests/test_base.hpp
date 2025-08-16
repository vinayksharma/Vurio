#pragma once

#include <gtest/gtest.h>
#include <filesystem>
#include <string>
#include <thread>
#include <chrono>
#include <fstream>
#include "database/database_manager.hpp"
#include "core/server_config_manager.hpp"
#include "logging/logger.hpp"

/**
 * @brief Base class for all tests that provides common test environment setup
 */
class TestBase : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Set test mode environment variable
        setenv("TEST_MODE", "1", 1);

        // Create test database directory
        test_db_dir_ = std::filesystem::temp_directory_path() / "dedup_test_db";
        std::filesystem::create_directories(test_db_dir_);

        // Set test database path
        test_db_path_ = (test_db_dir_ / "test_database.db").string();
        setenv("TEST_DB_PATH", test_db_path_.c_str(), 1);

        // Create test files directory
        test_files_dir_ = std::filesystem::temp_directory_path() / "dedup_test_files";
        std::filesystem::create_directories(test_files_dir_);
        setenv("TEST_FILES_PATH", test_files_dir_.string().c_str(), 1);

        // Ensure DatabaseManager is completely reset for this test
        DatabaseManager::resetForTesting();

        // Wait a moment to ensure cleanup is complete
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        // Initialize DatabaseManager with the test path
        DatabaseManager::getInstance(test_db_path_);

        // Initialize ServerConfigManager for tests
        ServerConfigManager::getInstance().loadConfig("config.yaml"); // Load default config for tests

        Logger::info("TestBase SetUp completed for test: " + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    }

    void TearDown() override
    {
        // Skip database cleanup for now to prevent SEGFAULTs
        // TODO: Investigate why DatabaseManager::shutdown() causes SEGFAULTs

        // Try to shut down the database manager more carefully
        try
        {
            // Wait a moment to ensure all operations are complete
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

            // Reset the DatabaseManager for the next test
            DatabaseManager::resetForTesting();

            // Wait a moment for cleanup to complete
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        catch (const std::exception &e)
        {
            // Ignore any cleanup errors to prevent crashes
        }

        // Clean up test files directory
        try
        {
            if (std::filesystem::exists(test_files_dir_))
            {
                std::filesystem::remove_all(test_files_dir_);
            }
        }
        catch (const std::exception &e)
        {
            // Ignore any cleanup errors to prevent crashes
        }

        // Unset test mode environment variable
        try
        {
            unsetenv("TEST_MODE");
            unsetenv("TEST_DB_PATH");
            unsetenv("TEST_FILES_PATH");
        }
        catch (const std::exception &e)
        {
            // Ignore any environment variable cleanup errors
        }

        Logger::info("TestBase TearDown completed for test: " + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    }

    // Helper to create a dummy file for tests
    void createDummyFile(const std::string &filename, const std::string &content = "dummy content")
    {
        std::filesystem::path file_path = test_files_dir_ / filename;
        std::ofstream ofs(file_path);
        ofs << content;
        ofs.close();
    }

    // Helper method to get test database path
    std::string getTestDbPath() const { return test_db_path_; }

    // Helper method to get test files directory
    std::string getTestFilesDir() const { return test_files_dir_.string(); }

private:
    std::filesystem::path test_db_dir_;
    std::filesystem::path test_files_dir_;
    std::string test_db_path_;
};
