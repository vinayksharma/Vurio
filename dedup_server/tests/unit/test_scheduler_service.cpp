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

// QW-3 (backend_optimization_plan.md): pausing the scheduler is the mechanism
// that aborts in-flight scans/dedup runs (ServerInitializer wires abort calls
// into the pause callback — and must do so only after those objects exist).
// These tests pin the callback contract that wiring relies on.
TEST(SchedulerServiceTest, PauseInvokesPauseCallbackEachTime)
{
    auto cfg = std::make_shared<UnifiedObservableConfigManager>("", false);
    ASSERT_TRUE(cfg->initialize());
    auto tpm = std::make_shared<ThreadPoolManager>(cfg);
    tpm->initialize();

    SchedulerService svc(cfg, tpm);
    svc.start();

    std::atomic<int> aborts{0};
    svc.setPauseCallback([&]()
                         { ++aborts; });

    svc.pause();
    EXPECT_EQ(aborts.load(), 1) << "pause() must invoke the abort callback";

    svc.resume();
    EXPECT_EQ(aborts.load(), 1) << "resume() must not invoke the abort callback";

    svc.pause();
    EXPECT_EQ(aborts.load(), 2) << "every pause() must re-invoke the abort callback";

    svc.resume();
    svc.stop();
}

TEST(SchedulerServiceTest, PauseWithoutCallbackIsSafe)
{
    auto cfg = std::make_shared<UnifiedObservableConfigManager>("", false);
    ASSERT_TRUE(cfg->initialize());
    auto tpm = std::make_shared<ThreadPoolManager>(cfg);
    tpm->initialize();

    SchedulerService svc(cfg, tpm);
    svc.start();

    // No callback wired: pause/resume must be a no-op, not a crash.
    svc.pause();
    svc.resume();
    svc.stop();
    SUCCEED();
}

#ifdef STANDALONE_MAIN_SCHED
int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
#endif
