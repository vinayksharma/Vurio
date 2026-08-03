#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>

#include "orchestration/scheduler_service.hpp"
#include "orchestration/thread_pool_manager.hpp"
#include "test_utils.hpp"
#include "config/unified_observable_config.hpp"

using namespace MediaDedup;
using namespace MediaDedup::Orchestration;

TEST(SchedulerServiceTest, StopPreventsFutureDispatch)
{
    auto cfg = std::make_shared<UnifiedObservableConfigManager>("", false);
    ASSERT_TRUE(cfg->initialize());
    auto tpm = std::make_shared<ThreadPoolManager>(cfg);
    tpm->initialize();

    auto svc_ptr = std::make_shared<SchedulerService>(cfg, tpm);
    SchedulerService &svc = *svc_ptr;
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

    auto svc_ptr = std::make_shared<SchedulerService>(cfg, tpm);
    SchedulerService &svc = *svc_ptr;
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

    auto svc_ptr = std::make_shared<SchedulerService>(cfg, tpm);
    SchedulerService &svc = *svc_ptr;
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

// CC-4: registering an id twice must be rejected — the old code overwrote the
// unique_ptr, destroying a live Job whose worker thread still held a pointer
// into it (use-after-free, then std::terminate from the joinable dtor).
TEST(SchedulerServiceTest, DuplicateRegistrationIsRejected)
{
    auto cfg = std::make_shared<UnifiedObservableConfigManager>("", false);
    ASSERT_TRUE(cfg->initialize());
    auto tpm = std::make_shared<ThreadPoolManager>(cfg);
    tpm->initialize();

    auto svc = std::make_shared<SchedulerService>(cfg, tpm);
    svc->start();

    std::atomic<int> first{0}, second{0};
    svc->registerJob("dup", std::chrono::hours(1), "sched", [&]() { ++first; });
    svc->registerJob("dup", std::chrono::hours(1), "sched", [&]() { ++second; });

    EXPECT_EQ(svc->listJobs().size(), 1u);

    // The surviving registration is the FIRST one.
    ASSERT_TRUE(svc->triggerJob("dup"));
    bool ran = MediaDedup::Test::TestUtils::waitForCondition(
        [&]() { return first.load() == 1; },
        std::chrono::milliseconds(2000), std::chrono::milliseconds(10));
    EXPECT_TRUE(ran);
    EXPECT_EQ(second.load(), 0) << "duplicate registration must not replace the job";

    svc->stop();
    tpm->shutdownAndDrain(std::chrono::milliseconds(500));
}

// CC-4: the IDLE->RUNNING claim is a single compare_exchange — a second
// dispatch while the job runs must be refused, and the job must be
// triggerable again once idle.
TEST(SchedulerServiceTest, TriggerWhileRunningIsRejectedAtomically)
{
    auto cfg = std::make_shared<UnifiedObservableConfigManager>("", false);
    ASSERT_TRUE(cfg->initialize());
    auto tpm = std::make_shared<ThreadPoolManager>(cfg);
    tpm->initialize();

    auto svc = std::make_shared<SchedulerService>(cfg, tpm);
    svc->start();

    std::atomic<bool> release{false};
    std::atomic<int> runs{0};
    svc->registerJob("blocky", std::chrono::hours(1), "sched", [&]()
                     {
        ++runs;
        while (!release.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(5)); });

    ASSERT_TRUE(svc->triggerJob("blocky"));
    ASSERT_TRUE(MediaDedup::Test::TestUtils::waitForCondition(
        [&]() { return runs.load() == 1; },
        std::chrono::milliseconds(2000), std::chrono::milliseconds(10)));

    EXPECT_FALSE(svc->triggerJob("blocky")) << "second dispatch while RUNNING must be refused";
    EXPECT_TRUE(svc->isJobRunning("blocky"));

    release.store(true);
    ASSERT_TRUE(MediaDedup::Test::TestUtils::waitForCondition(
        [&]() { return !svc->isJobRunning("blocky"); },
        std::chrono::milliseconds(2000), std::chrono::milliseconds(10)));

    // Idle again: a new trigger is accepted and runs.
    release.store(false);
    EXPECT_TRUE(svc->triggerJob("blocky"));
    ASSERT_TRUE(MediaDedup::Test::TestUtils::waitForCondition(
        [&]() { return runs.load() == 2; },
        std::chrono::milliseconds(2000), std::chrono::milliseconds(10)));

    release.store(true);
    svc->stop();
    tpm->shutdownAndDrain(std::chrono::milliseconds(500));
}

// CC-4: workers block on a CV between runs and while paused; stop() must
// return promptly even with hour-long intervals and while paused (and it
// joins OUTSIDE jobsMutex_ so completion recording can't collide with it).
TEST(SchedulerServiceTest, StopIsPromptWithLongIntervalsAndWhilePaused)
{
    auto cfg = std::make_shared<UnifiedObservableConfigManager>("", false);
    ASSERT_TRUE(cfg->initialize());
    auto tpm = std::make_shared<ThreadPoolManager>(cfg);
    tpm->initialize();

    auto svc = std::make_shared<SchedulerService>(cfg, tpm);
    svc->start();
    svc->registerJob("longA", std::chrono::hours(1), "sched", []() {});
    svc->registerJob("longB", std::chrono::hours(1), "sched", []() {});
    svc->pause();

    const auto t0 = std::chrono::steady_clock::now();
    svc->stop();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    EXPECT_LT(ms, 1000) << "stop() must wake CV-blocked workers immediately";

    svc->resume(); // resume after stop is a harmless no-op
    tpm->shutdownAndDrain(std::chrono::milliseconds(200));
}
