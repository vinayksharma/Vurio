#include <gtest/gtest.h>
#include <filesystem>
#include <string>

#include "config/unified_observable_config.hpp"
#include "database/database_service.hpp"
#include "database/database_manager.hpp"
#include "test_utils.hpp"
#include <Poco/Data/SQLite/Connector.h>

using MediaDedup::DatabaseService;
using MediaDedup::DatabaseManager;
using MediaDedup::UnifiedObservableConfigManager;
namespace fs = std::filesystem;

TEST(DatabaseServiceTest, CreatesDatabaseFileAtConfiguredPath)
{
    // Prepare temp directory and config file
    std::string tempDir = MediaDedup::Test::TestUtils::generateTempDirectory("db_service_test");
    std::string cfgPath = tempDir + "/config.yaml";
    std::string dbPath = tempDir + "/test_db.sqlite";

    // Write minimal config with database.path
    MediaDedup::Test::TestUtils::createTempFile("database.path: " + dbPath + "\n", cfgPath);

    auto cfg = std::make_shared<UnifiedObservableConfigManager>(cfgPath, false, std::chrono::milliseconds(500));
    ASSERT_TRUE(cfg->initialize());

    DatabaseService service(cfg);
    ASSERT_TRUE(service.ensureDatabaseFileExists());
    EXPECT_TRUE(fs::exists(dbPath));

    // Cleanup
    MediaDedup::Test::TestUtils::deleteFile(dbPath);
    MediaDedup::Test::TestUtils::deleteFile(cfgPath);
    MediaDedup::Test::TestUtils::deleteDirectory(tempDir);
}

TEST(DatabaseServiceTest, CreatesParentDirectoriesIfMissing)
{
    std::string tempDir = MediaDedup::Test::TestUtils::generateTempDirectory("db_service_dirs");
    std::string cfgPath = tempDir + "/config.yaml";
    std::string nestedDir = tempDir + "/a/b/c";
    std::string dbPath = nestedDir + "/nested_db.sqlite";

    MediaDedup::Test::TestUtils::createTempFile("database.path: " + dbPath + "\n", cfgPath);

    auto cfg = std::make_shared<UnifiedObservableConfigManager>(cfgPath, false, std::chrono::milliseconds(500));
    ASSERT_TRUE(cfg->initialize());

    DatabaseService service(cfg);
    ASSERT_TRUE(service.ensureDatabaseFileExists());
    EXPECT_TRUE(fs::exists(nestedDir));
    EXPECT_TRUE(fs::exists(dbPath));

    // Cleanup
    MediaDedup::Test::TestUtils::deleteFile(dbPath);
    MediaDedup::Test::TestUtils::deleteFile(cfgPath);
    MediaDedup::Test::TestUtils::deleteDirectory(tempDir);
}

TEST(DatabaseManagerTest, BackupDatabaseCreatesValidBackup)
{
    std::string tempDir = MediaDedup::Test::TestUtils::generateTempDirectory("db_backup_test");
    MediaDedup::Test::TestUtils::createTempDirectory(tempDir);
    std::string dbPath = tempDir + "/main_db.sqlite";
    std::string backupPath = tempDir + "/backup_db.sqlite";

    // Setup basic DB manager
    Poco::Data::SQLite::Connector::registerConnector();
    auto db_manager = std::make_shared<DatabaseManager>(dbPath);
    ASSERT_TRUE(db_manager->initialize());

    std::cout << "Creating dummy table..." << std::endl;
    // Create a dummy table and insert data
    {
        auto lease = db_manager->acquireSessionLease();
        Poco::Data::Session& sess = lease.get();
        sess << "CREATE TABLE test_table (id INTEGER PRIMARY KEY, value TEXT)", Poco::Data::Keywords::now;
        sess << "INSERT INTO test_table (value) VALUES ('hello backup')", Poco::Data::Keywords::now;
    }

    std::cout << "Performing backup to " << backupPath << "..." << std::endl;
    // Perform backup
    bool backup_res = db_manager->backupDatabase(backupPath);
    std::cout << "Backup result: " << backup_res << std::endl;
    ASSERT_TRUE(backup_res);
    EXPECT_TRUE(fs::exists(backupPath));

    std::cout << "Verifying backup contents..." << std::endl;
    // Verify backup contents
    auto backup_manager = std::make_shared<DatabaseManager>(backupPath);
    std::cout << "Initializing backup manager..." << std::endl;
    ASSERT_TRUE(backup_manager->initialize());
    std::cout << "Backup manager initialized." << std::endl;
    
    auto lease = backup_manager->acquireSessionLease();
    Poco::Data::Session& sess = lease.get();
    Poco::Data::Statement stmt(sess);
    
    std::string value;
    stmt << "SELECT value FROM test_table WHERE id = 1", Poco::Data::Keywords::into(value), Poco::Data::Keywords::now;
    
    EXPECT_EQ(value, "hello backup");

    // Cleanup
    MediaDedup::Test::TestUtils::deleteDirectory(tempDir);
}

#if !defined(ALL_UNIT_TESTS)
int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
#endif
