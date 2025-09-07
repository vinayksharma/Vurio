#include <gtest/gtest.h>
#include <filesystem>
#include <string>

#include "config/unified_observable_config.hpp"
#include "database/database_service.hpp"
#include "test_utils.hpp"

using MediaDedup::DatabaseService;
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

#if !defined(ALL_UNIT_TESTS)
int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
#endif
