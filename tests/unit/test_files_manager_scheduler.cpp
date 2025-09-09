#include <gtest/gtest.h>
#include <atomic>
#include <thread>

#include "orchestration/files_manager.hpp"
#include "orchestration/scheduler_service.hpp"
#include "orchestration/thread_pool_manager.hpp"
#include "config/unified_observable_config.hpp"
#include "database/database_manager.hpp"
#include "filesmanager/files_service.hpp"

using namespace MediaDedup;
using namespace MediaDedup::Orchestration;

TEST(FilesManagerTest, NonOverlapRunOnce)
{
    auto cfg = std::make_shared<UnifiedObservableConfigManager>("", false);
    ASSERT_TRUE(cfg->initialize());

    // Use a temp db path
    std::string dbPath = (std::filesystem::temp_directory_path() / "mds_files_mgr_test.db").string();
    auto db = std::make_shared<DatabaseManager>(dbPath);
    ASSERT_TRUE(db->initialize());

    FilesService filesService(*db);
    auto filesServicePtr = std::make_shared<FilesService>(*db);

    FilesManager fm(cfg, db, filesServicePtr);
    fm.initialize();

    // Launch two runOnce calls concurrently; second should be skipped
    std::thread t1([&]()
                   { fm.runOnce(); });
    std::thread t2([&]()
                   { fm.runOnce(); });
    t1.join();
    t2.join();

    SUCCEED();
}

#ifdef STANDALONE_MAIN_FILESFM
int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
#endif
