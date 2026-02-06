#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>

#include "orchestration/scheduler_service.hpp"
#include "orchestration/thread_pool_manager.hpp"
#include "config/unified_observable_config.hpp"

using namespace MediaDedup;
using namespace MediaDedup::Orchestration;

TEST(SchedulerServiceTest, StopPreventsFutureDispatch)
{
    auto cfg = std::make_shared<UnifiedObservableConfigManager>("", false);
    ASSERT_TRUE(cfg->initialize());
    auto tpm = std::make_shared<ThreadPoolManager>(cfg);
    tpm->initialize();

    SchedulerService svc(cfg, tpm);
    svc.start();

    std::atomic<int> count{0};
    svc.registerJob("j1", std::chrono::milliseconds(50), "sched", [&]()
                    { ++count; });

    std::this_thread::sleep_for(std::chrono::milliseconds(160));
    svc.stop();
    int afterStop = count.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_GE(afterStop, 1);
    EXPECT_EQ(count.load(), afterStop) << "No new dispatches after stop";
}

#ifdef STANDALONE_MAIN_SCHED
int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
#endif
