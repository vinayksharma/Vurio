#include "database/database_manager.hpp"
#include <iostream>
#include <filesystem>
#include <cstdlib>
#include "utils/logger.hpp" // Added for Logger::info

int main()
{
    try
    {
        std::cout << "Setting up test environment..." << std::endl;

        // Set test-specific environment variables
        setenv("TEST_MODE", "1", 1);

        // Create test database directory
        std::filesystem::path test_db_dir = std::filesystem::temp_directory_path() / "dedup_test_db";
        std::filesystem::create_directories(test_db_dir);

        // Set test database path
        std::string test_db_path = (test_db_dir / "test_database.db").string();
        setenv("TEST_DB_PATH", test_db_path.c_str(), 1);

        // Initialize DatabaseManager with test database
        auto &db_manager = DatabaseManager::getInstance(test_db_path);

        // Note: ServerConfigManager initialization would require more dependencies
        // For now, just set up the database

        Logger::info("Test environment setup complete.");
        Logger::info("Test database: " + test_db_path);

        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Test environment setup failed: " << e.what() << std::endl;
        return 1;
    }
}
