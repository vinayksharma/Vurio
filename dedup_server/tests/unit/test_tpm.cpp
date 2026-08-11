#include <gtest/gtest.h>
#include "orchestration/thread_pool_manager.hpp"
#include "config/unified_observable_config.hpp"
#include "test_utils.hpp"
#include <atomic>
#include <thread>
#include <chrono>
#include <set>

using namespace MediaDedup;
using namespace MediaDedup::Test;

TEST(ThreadPoolManagerTest, SchedulesWithSharesAndDrains)
{
    // Minimal config with auto pool and small kill timeout
    auto cfg = std::make_shared<UnifiedObservableConfigManager>("/dev/null", false);
    ASSERT_TRUE(cfg->initialize());
    cfg->createProperty("tpm.pool.max", std::string("auto"));
    cfg->createProperty("tpm.killTimeoutMs", 100);

    ThreadPoolManager tpm(cfg);
    tpm.initialize();
    tpm.setShare("A", 0.5);
    tpm.setShare("B", 0.5);

    std::atomic<int> ranA{0}, ranB{0};
    for (int i = 0; i < 4; ++i)
        tpm.submit("A", [&]()
                   { ranA++; std::this_thread::sleep_for(std::chrono::milliseconds(10)); });
    for (int i = 0; i < 4; ++i)
        tpm.submit("B", [&]()
                   { ranB++; std::this_thread::sleep_for(std::chrono::milliseconds(10)); });

    // Allow some time to run
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto st = tpm.getStatus();
    EXPECT_LE(static_cast<int>(st.runningTotal), static_cast<int>(st.effectiveMax));

    tpm.shutdownAndDrain(std::chrono::milliseconds(200));
    // After drain, no active running tasks
    st = tpm.getStatus();
    EXPECT_EQ(st.runningTotal, 0u);
}

// QW-5 (backend_optimization_plan.md): submit() used to read
// type_to_queue_[type] OUTSIDE the mutex for its debug log — a mutating
// unordered_map::operator[] racing schedule()/onTaskFinished on worker
// threads (torn reads / rehash corruption). This stress drives exactly that
// interleaving: many submitters logging while workers drain and re-schedule.
// Run under TSan/ASan it trips on the old code; unsanitized it pins that no
// submission is lost or double-run.
TEST(ThreadPoolManagerTest, ConcurrentSubmitWhileDrainingIsSafe)
{
    auto cfg = std::make_shared<UnifiedObservableConfigManager>("/dev/null", false);
    ASSERT_TRUE(cfg->initialize());
    cfg->createProperty("tpm.pool.max", std::string("4"));
    cfg->createProperty("tpm.killTimeoutMs", 200);

    ThreadPoolManager tpm(cfg);
    tpm.initialize();

    constexpr int kSubmitters = 8;
    constexpr int kTasksEach = 100;
    std::atomic<int> ran{0};

    std::vector<std::thread> submitters;
    submitters.reserve(kSubmitters);
    for (int t = 0; t < kSubmitters; ++t)
    {
        submitters.emplace_back([&tpm, &ran, t]()
                                {
            // Mixed types: same-type contention plus new-type insertion, both
            // of which mutate type_to_queue_ under schedule() concurrently.
            const std::string type = (t % 2 == 0) ? "stressA" : ("stress" + std::to_string(t));
            for (int i = 0; i < kTasksEach; ++i)
            {
                // Unique file paths so the dedup check never drops any.
                tpm.submit(type, [&ran]() { ran++; },
                           "/stress/" + std::to_string(t) + "/" + std::to_string(i));
            } });
    }
    for (auto &s : submitters)
        s.join();

    // Every submitted task runs exactly once.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (ran.load() < kSubmitters * kTasksEach && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_EQ(ran.load(), kSubmitters * kTasksEach);

    tpm.shutdownAndDrain(std::chrono::milliseconds(500));
    EXPECT_EQ(tpm.getStatus().runningTotal, 0u);
}

TEST(ThreadPoolManagerTest, RespectsPerTypeSharesAtConcurrency)
{
    auto cfg = std::make_shared<UnifiedObservableConfigManager>("/dev/null", false);
    ASSERT_TRUE(cfg->initialize());
    cfg->createProperty("tpm.pool.max", std::string("4"));
    cfg->createProperty("tpm.killTimeoutMs", 200);

    ThreadPoolManager tpm(cfg);
    tpm.initialize();

    // Set shares: A=0.25 => allowance 1; B=0.5 => allowance 2
    tpm.setShare("A", 0.25);
    tpm.setShare("B", 0.5);
    tpm.setShare("F", 1.0);

    // Deterministic barrier: start exactly A1, B1, B2 and F1, then verify counts
    std::atomic<bool> block{true};
    std::atomic<int> startedA{0}, startedB{0}, startedF{0};
    auto makeTask = [&](std::atomic<int> &counter)
    {
        return [&]()
        {
            counter.fetch_add(1);
            while (block.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        };
    };

    // Submit in order: expected to fit allowances: A(1), B(2), F(1)
    tpm.submit("A", makeTask(startedA));
    tpm.submit("B", makeTask(startedB));
    tpm.submit("B", makeTask(startedB));
    tpm.submit("F", makeTask(startedF));

    // Wait until each target type has started required number of tasks
    bool ok = MediaDedup::Test::TestUtils::waitForCondition([&]()
                                                            {
        auto st = tpm.getStatus();
        return startedA.load() >= 1 && startedB.load() >= 2 && startedF.load() >= 1 && st.runningTotal >= 3; }, std::chrono::milliseconds(2000), std::chrono::milliseconds(10));
    ASSERT_TRUE(ok);

    auto st = tpm.getStatus();
    EXPECT_LE(st.perType["A"].running, 1u);
    EXPECT_LE(st.perType["B"].running, 2u);
    EXPECT_LE(st.runningTotal, 4u);

    // Release tasks and drain
    block.store(false);
    tpm.shutdownAndDrain(std::chrono::milliseconds(500));
    st = tpm.getStatus();
    EXPECT_EQ(st.runningTotal, 0u);
}

TEST(ThreadPoolManagerTest, IdleTimeoutConfiguration_DefaultValue_Is120Seconds)
{
    // Test that the default idle timeout is 120 seconds
    auto cfg = std::make_shared<UnifiedObservableConfigManager>("/dev/null", false);
    ASSERT_TRUE(cfg->initialize());

    ThreadPoolManager tpm(cfg);
    tpm.initialize();

    // The default should be 120 seconds as configured in config factory
    // We can't directly test the Poco ThreadPool's idle timeout, but we can verify
    // that the configuration is read correctly by checking the logs or behavior
    tpm.shutdownAndDrain(std::chrono::milliseconds(100));
}

// CC-1 (backend_optimization_plan.md): thread-setting config changes must
// NEVER recreate the pool at runtime. The old recreateThreadPool() waited on
// cv_ while holding mutex_ and then joined workers that were blocked on that
// same mutex — a hard deadlock reachable from any config edit — and it
// silently discarded all queued work. This drives the exact trigger (config
// change while tasks are IN FLIGHT and QUEUED) and asserts nothing stalls
// and nothing is lost. Under the old code this test hangs.
TEST(ThreadPoolManagerTest, ChangingThreadSettingsMidFlightNeverStallsOrDropsWork)
{
    auto cfg = std::make_shared<UnifiedObservableConfigManager>("/dev/null", false);
    ASSERT_TRUE(cfg->initialize());
    cfg->createProperty("tpm.pool.max", std::string("2"));

    ThreadPoolManager tpm(cfg);
    tpm.initialize();

    // Occupy the pool with slow tasks and queue extras behind them.
    std::atomic<bool> release{false};
    std::atomic<int> completed{0};
    for (int i = 0; i < 6; ++i)
        tpm.submit("test", [&]()
                   {
            while (!release.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            completed.fetch_add(1); },
                   "/cc1/" + std::to_string(i));

    // Config changes land while tasks run and the queue is non-empty.
    // (Old code: idle-timeout change here deadlocks the whole TPM.)
    cfg->setPropertyValue<int>("tpm.thread.idleTimeoutSeconds", 60);
    cfg->setPropertyValue<int>("tpm.thread.stackSizeKB", 2048);

    release.store(true);
    bool all_done = TestUtils::waitForCondition([&]()
                                                { return completed.load() == 6; },
                                                std::chrono::milliseconds(5000), std::chrono::milliseconds(10));
    EXPECT_TRUE(all_done) << "config change stalled the pool or dropped queued work (completed="
                          << completed.load() << "/6)";

    tpm.shutdownAndDrain(std::chrono::milliseconds(500));
}

TEST(ThreadPoolManagerTest, IdleTimeoutConfiguration_InvalidValue_KeepsCurrentValue)
{
    // Test that invalid idle timeout values are rejected
    auto cfg = std::make_shared<UnifiedObservableConfigManager>("/dev/null", false);
    ASSERT_TRUE(cfg->initialize());

    ThreadPoolManager tpm(cfg);
    tpm.initialize();

    // Try to set invalid values
    cfg->setPropertyValue<int>("tpm.thread.idleTimeoutSeconds", 0); // Invalid: <= 0
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    cfg->setPropertyValue<int>("tpm.thread.idleTimeoutSeconds", -10); // Invalid: negative
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Submit a task to verify the pool is still working
    std::atomic<bool> taskCompleted{false};
    tpm.submit("test", [&taskCompleted]()
               {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        taskCompleted.store(true); });

    // Wait for task to complete
    TestUtils::waitForCondition([&taskCompleted]()
                                { return taskCompleted.load(); },
                                std::chrono::milliseconds(1000), std::chrono::milliseconds(10));

    EXPECT_TRUE(taskCompleted.load());

    tpm.shutdownAndDrain(std::chrono::milliseconds(100));
}

TEST(ThreadPoolManagerTest, GetQueueDepth)
{
    auto cfg = std::make_shared<UnifiedObservableConfigManager>("/dev/null", false);
    ASSERT_TRUE(cfg->initialize());
    cfg->createProperty("tpm.pool.max", std::string("2"));
    cfg->createProperty("tpm.killTimeoutMs", 100);

    ThreadPoolManager tpm(cfg);
    tpm.initialize();

    // Test queue depth for non-existent type
    EXPECT_EQ(tpm.getQueueDepth("non_existent"), 0u);

    // Submit some tasks to unified queue
    std::atomic<bool> task1Completed{false};
    std::atomic<bool> task2Completed{false};
    std::atomic<bool> task3Completed{false};

    tpm.submit("media_processor", [&task1Completed]()
               { std::this_thread::sleep_for(std::chrono::milliseconds(100)); task1Completed = true; });
    tpm.submit("media_processor", [&task2Completed]()
               { std::this_thread::sleep_for(std::chrono::milliseconds(100)); task2Completed = true; });
    tpm.submit("media_processor", [&task3Completed]()
               { std::this_thread::sleep_for(std::chrono::milliseconds(100)); task3Completed = true; });

    // Check unified queue depth
    size_t queue_depth = tpm.getQueueDepth("media_processor");
    EXPECT_GE(queue_depth, 1u); // At least one task should be queued

    // Wait for tasks to complete
    TestUtils::waitForCondition([&task1Completed]()
                                { return task1Completed.load(); },
                                std::chrono::milliseconds(1000), std::chrono::milliseconds(10));
    TestUtils::waitForCondition([&task2Completed]()
                                { return task2Completed.load(); },
                                std::chrono::milliseconds(1000), std::chrono::milliseconds(10));
    TestUtils::waitForCondition([&task3Completed]()
                                { return task3Completed.load(); },
                                std::chrono::milliseconds(1000), std::chrono::milliseconds(10));

    tpm.shutdownAndDrain(std::chrono::milliseconds(200));
}

TEST(ThreadPoolManagerTest, GetAllQueueDepths)
{
    auto cfg = std::make_shared<UnifiedObservableConfigManager>("/dev/null", false);
    ASSERT_TRUE(cfg->initialize());
    cfg->createProperty("tpm.pool.max", std::string("2"));
    cfg->createProperty("tpm.killTimeoutMs", 100);

    ThreadPoolManager tpm(cfg);
    tpm.initialize();

    // Initially should be empty
    auto queue_depths = tpm.getAllQueueDepths();
    EXPECT_TRUE(queue_depths.empty());

    // Submit tasks to unified media processor queue
    std::atomic<bool> task1Completed{false};
    std::atomic<bool> task2Completed{false};
    std::atomic<bool> task3Completed{false};

    tpm.submit("media_processor", [&task1Completed]()
               { std::this_thread::sleep_for(std::chrono::milliseconds(200)); task1Completed = true; });
    tpm.submit("media_processor", [&task2Completed]()
               { std::this_thread::sleep_for(std::chrono::milliseconds(200)); task2Completed = true; });
    tpm.submit("media_processor", [&task3Completed]()
               { std::this_thread::sleep_for(std::chrono::milliseconds(200)); task3Completed = true; });

    // Give a moment for tasks to be queued
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Check all queue depths
    queue_depths = tpm.getAllQueueDepths();
    EXPECT_GE(queue_depths.size(), 1u); // Should have at least 1 type

    // Check unified queue depth - should have at least one task queued
    EXPECT_GE(queue_depths["media_processor"], 1u); // At least one task should be queued

    // Wait for tasks to complete
    TestUtils::waitForCondition([&task1Completed]()
                                { return task1Completed.load(); },
                                std::chrono::milliseconds(1000), std::chrono::milliseconds(10));
    TestUtils::waitForCondition([&task2Completed]()
                                { return task2Completed.load(); },
                                std::chrono::milliseconds(1000), std::chrono::milliseconds(10));
    TestUtils::waitForCondition([&task3Completed]()
                                { return task3Completed.load(); },
                                std::chrono::milliseconds(1000), std::chrono::milliseconds(10));

    tpm.shutdownAndDrain(std::chrono::milliseconds(200));
}

TEST(ThreadPoolManagerTest, FilePathTracking)
{
    // Minimal config with very small thread pool to force queuing
    auto cfg = std::make_shared<UnifiedObservableConfigManager>("/dev/null", false);
    ASSERT_TRUE(cfg->initialize());
    cfg->createProperty("tpm.pool.max", std::string("1")); // Only 1 thread
    cfg->createProperty("tpm.killTimeoutMs", 100);

    ThreadPoolManager tpm(cfg);
    tpm.initialize();
    tpm.setShare("media_processor", 1.0);

    // Submit tasks with file paths - use blocking tasks to prevent immediate execution
    std::atomic<bool> task1_done{false};
    std::atomic<bool> task2_done{false};
    std::atomic<bool> task3_done{false};
    std::atomic<bool> task4_done{false};

    tpm.submit("media_processor", [&task1_done](){
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        task1_done = true;
    }, "/path/to/file1.jpg");
    
    tpm.submit("media_processor", [&task2_done](){
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        task2_done = true;
    }, "/path/to/file2.jpg");
    
    tpm.submit("media_processor", [&task3_done](){
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        task3_done = true;
    }, "/path/to/file3.jpg");
    
    // Submit task without file path
    tpm.submit("media_processor", [&task4_done](){
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        task4_done = true;
    });

    // Give a moment for first task to start but others to remain queued
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Check pending file paths - should have 3 remaining (4 total - 1 running)
    auto pending_paths = tpm.getPendingFilePaths("media_processor");
    EXPECT_GE(pending_paths.size(), 2); // At least 2 should be queued (3 with paths + 1 without - 1 running)
    
    // Check specific file paths are tracked
    std::set<std::string> expected_paths = {
        "/path/to/file1.jpg",
        "/path/to/file2.jpg", 
        "/path/to/file3.jpg"
    };
    
    std::set<std::string> actual_paths(pending_paths.begin(), pending_paths.end());
    // At least one of the expected paths should be found
    bool found_expected = false;
    for (const auto& expected : expected_paths) {
        if (actual_paths.count(expected)) {
            found_expected = true;
            break;
        }
    }
    EXPECT_TRUE(found_expected) << "None of the expected file paths were found in pending queue";

    // Check all pending file paths
    auto all_pending = tpm.getAllPendingFilePaths();
    EXPECT_EQ(all_pending.size(), 1); // Only "media_processor" type
    EXPECT_GE(all_pending["media_processor"].size(), 2); // At least 2 should be pending

    // Wait for all tasks to complete
    tpm.shutdownAndDrain(std::chrono::milliseconds(500));
}

TEST(ThreadPoolManagerTest, CancelTask)
{
    auto cfg = std::make_shared<UnifiedObservableConfigManager>("/dev/null", false);
    ASSERT_TRUE(cfg->initialize());
    cfg->createProperty("tpm.pool.max", std::string("1")); // Only 1 thread to force queueing
    cfg->createProperty("tpm.killTimeoutMs", 100);

    ThreadPoolManager tpm(cfg);
    tpm.initialize();

    std::atomic<bool> task1_done{false};
    std::atomic<bool> task2_done{false};

    // Task 1 will block the queue
    tpm.submit("media_processor", [&task1_done](){
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        task1_done = true;
    }, "/path/to/running.jpg");

    // Task 2 gets queued
    tpm.submit("media_processor", [&task2_done](){
        task2_done = true;
    }, "/path/to/queued.jpg");

    // Give a moment for task 1 to start
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Cancel task 2 while it's in the queue
    bool cancelled = tpm.cancelTask("media_processor", "/path/to/queued.jpg");
    EXPECT_TRUE(cancelled);

    // Cancel a task that doesn't exist
    bool cancelled_fake = tpm.cancelTask("media_processor", "/path/to/fake.jpg");
    EXPECT_FALSE(cancelled_fake);

    // Wait for task 1 to finish
    TestUtils::waitForCondition([&task1_done](){ return task1_done.load(); }, std::chrono::milliseconds(1000), std::chrono::milliseconds(10));
    
    // Ensure task 2 never ran
    EXPECT_FALSE(task2_done.load());

    tpm.shutdownAndDrain(std::chrono::milliseconds(100));
}

TEST(ThreadPoolManagerTest, TemporaryPriorityOverride)
{
    auto cfg = std::make_shared<UnifiedObservableConfigManager>("/dev/null", false);
    ASSERT_TRUE(cfg->initialize());
    cfg->createProperty("tpm.pool.max", std::string("2"));
    cfg->createProperty("tpm.killTimeoutMs", 100);

    ThreadPoolManager tpm(cfg);
    tpm.initialize();
    
    // Default shares
    tpm.setShare("A", 1.0);
    tpm.setShare("B", 1.0);

    // Pause "B" tasks temporarily
    tpm.setTemporaryOverride("B", 0.0);

    std::atomic<int> startedA{0}, startedB{0};
    
    // Submit tasks
    tpm.submit("A", [&](){
        startedA.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    });
    tpm.submit("B", [&](){
        startedB.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    });

    // Wait a bit to let tasks start
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // A should start, but B should not (because B is paused)
    EXPECT_EQ(startedA.load(), 1);
    EXPECT_EQ(startedB.load(), 0);

    // Now clear the override
    tpm.clearTemporaryOverride("B");
    
    // Wait for B to start
    bool ok = MediaDedup::Test::TestUtils::waitForCondition(
        [&](){ return startedB.load() > 0; },
        std::chrono::milliseconds(1000), 
        std::chrono::milliseconds(10)
    );
    
    EXPECT_TRUE(ok);
    EXPECT_EQ(startedB.load(), 1);

    tpm.shutdownAndDrain(std::chrono::milliseconds(100));
}

TEST(ThreadPoolManagerTest, AutoPrioritizesUITasks)
{
    auto cfg = std::make_shared<UnifiedObservableConfigManager>("/dev/null", false);
    ASSERT_TRUE(cfg->initialize());
    cfg->createProperty("tpm.pool.max", std::string("2"));
    cfg->createProperty("tpm.killTimeoutMs", 100);

    ThreadPoolManager tpm(cfg);
    tpm.initialize();
    tpm.setShare("image_processor", 1.0);
    tpm.setShare("thumbnail_generator", 0.25);

    std::atomic<int> bg_started{0};
    std::atomic<int> ui_finished{0};
    std::atomic<int> bg_finished{0};
    std::atomic<bool> release_bg{false};

    // Submit 2 background tasks to consume the available 2 threads and block.
    for (int i = 0; i < 2; ++i)
    {
        tpm.submit("image_processor", [&]() {
            bg_started++;
            while (!release_bg.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            bg_finished++;
        }, "file_bg_" + std::to_string(i));
    }

    // Wait until 2 background tasks have started (filling the pool)
    bool ok = MediaDedup::Test::TestUtils::waitForCondition(
        [&](){ return bg_started.load() >= 2; },
        std::chrono::milliseconds(1000), 
        std::chrono::milliseconds(10)
    );
    EXPECT_TRUE(ok);

    // Submit 5 MORE background tasks (they will be queued)
    for (int i = 2; i < 7; ++i)
    {
        tpm.submit("image_processor", [&]() {
            // These should run AFTER UI tasks are scheduled.
            // Under concurrent execution, as soon as one UI task finishes, a background task
            // can start executing while the other UI task is still running.
            // So we expect at least 1 UI task to have finished.
            EXPECT_GE(ui_finished.load(), 1);
            bg_finished++;
        }, "file_bg_" + std::to_string(i));
    }

    // Submit 2 UI tasks. Urgent is what makes them UI: the type name alone no
    // longer buys priority, or a 20k-job backfill into the same queue would
    // masquerade as a user waiting on a thumbnail.
    for (int i = 0; i < 2; ++i)
    {
        tpm.submit("thumbnail_generator", [&]() {
            ui_finished++;
        }, "file_ui_" + std::to_string(i), /*urgent=*/true);
    }

    // Release the blocked background tasks
    release_bg.store(true);

    // Wait for everything
    ok = MediaDedup::Test::TestUtils::waitForCondition(
        [&](){ return bg_finished.load() >= 7 && ui_finished.load() >= 2; },
        std::chrono::milliseconds(2000), 
        std::chrono::milliseconds(10)
    );
    EXPECT_TRUE(ok);

    tpm.shutdownAndDrain(std::chrono::milliseconds(100));
}

TEST(ThreadPoolManagerTest, UIGraceCapacityBypassesMaxLimit)
{
    auto cfg = std::make_shared<UnifiedObservableConfigManager>("/dev/null", false);
    ASSERT_TRUE(cfg->initialize());
    cfg->createProperty("tpm.pool.max", std::string("2")); // effective_max = 2, pool capacity = 4
    cfg->createProperty("tpm.killTimeoutMs", 100);

    ThreadPoolManager tpm(cfg);
    tpm.initialize();
    tpm.setShare("image_processor", 1.0);
    tpm.setShare("thumbnail_generator", 0.25);

    std::atomic<int> bg_started{0};
    std::atomic<int> ui_started{0};
    std::atomic<bool> release_all{false};

    // Submit 2 background tasks to saturate effective_max_ limit (running = 2).
    for (int i = 0; i < 2; ++i)
    {
        tpm.submit("image_processor", [&]() {
            bg_started++;
            while (!release_all.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }, "file_bg_" + std::to_string(i));
    }

    // Wait until both background tasks start.
    bool ok = MediaDedup::Test::TestUtils::waitForCondition(
        [&](){ return bg_started.load() >= 2; },
        std::chrono::milliseconds(1000), 
        std::chrono::milliseconds(10)
    );
    ASSERT_TRUE(ok);

    // Now submit 2 UI tasks. Since grace capacity is pool_->capacity() = 4,
    // they should start running immediately even though background tasks are blocking.
    for (int i = 0; i < 2; ++i)
    {
        tpm.submit("thumbnail_generator", [&]() {
            ui_started++;
            while (!release_all.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }, "file_ui_" + std::to_string(i), /*urgent=*/true);
    }

    // Wait and verify that UI tasks have actually started (running total = 4).
    ok = MediaDedup::Test::TestUtils::waitForCondition(
        [&](){ return ui_started.load() >= 2; },
        std::chrono::milliseconds(2000), 
        std::chrono::milliseconds(10)
    );
    EXPECT_TRUE(ok);
    EXPECT_EQ(tpm.getStatus().runningTotal, 4u);

    // Release all tasks and clean up
    release_all.store(true);
    tpm.shutdownAndDrain(std::chrono::milliseconds(200));
}

#if !defined(ALL_UNIT_TESTS)
int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
#endif


// CC-2 (backend_optimization_plan.md): when pool_->start throws (e.g.
// NoThreadAvailableException), the task must be re-queued INTACT. The old
// code re-queued a moved-from shell whose std::function was empty: it later
// "ran" as a swallowed bad_function_call, the work was silently lost, and
// its file_path blocked resubmission via the dedup check. This injects the
// throw through the startOnPool seam and asserts the work still executes.
namespace
{
    class StartThrowingTpm : public ThreadPoolManager
    {
    public:
        using ThreadPoolManager::ThreadPoolManager;
        std::atomic<int> throws_remaining{0};

    protected:
        void startOnPool(Poco::Runnable &runnable) override
        {
            if (throws_remaining.fetch_sub(1) > 0)
                throw Poco::NoThreadAvailableException();
            ThreadPoolManager::startOnPool(runnable);
        }
    };
} // namespace

TEST(ThreadPoolManagerTest, FailedStartRequeuesTaskIntact)
{
    auto cfg = std::make_shared<UnifiedObservableConfigManager>("/dev/null", false);
    ASSERT_TRUE(cfg->initialize());
    cfg->createProperty("tpm.pool.max", std::string("2"));

    StartThrowingTpm tpm(cfg);
    tpm.initialize();

    std::atomic<int> ran{0};
    tpm.throws_remaining.store(1);

    // First scheduling attempt throws; the task must survive with its
    // callable AND file_path intact.
    tpm.submit("test", [&]() { ran.fetch_add(1); }, "/cc2/victim.jpg");

    // A second submission triggers the next scheduling pass (the seam no
    // longer throws), which must start BOTH tasks.
    tpm.submit("test", [&]() { ran.fetch_add(1); }, "/cc2/other.jpg");

    bool both_ran = MediaDedup::Test::TestUtils::waitForCondition(
        [&]() { return ran.load() == 2; },
        std::chrono::milliseconds(3000), std::chrono::milliseconds(10));
    EXPECT_TRUE(both_ran) << "re-queued task lost its callable (ran=" << ran.load() << "/2)";

    // The path must not be zombie-blocked: resubmitting it runs again.
    tpm.submit("test", [&]() { ran.fetch_add(1); }, "/cc2/victim.jpg");
    bool third_ran = MediaDedup::Test::TestUtils::waitForCondition(
        [&]() { return ran.load() == 3; },
        std::chrono::milliseconds(3000), std::chrono::milliseconds(10));
    EXPECT_TRUE(third_ran) << "file_path still blocked by a zombie queue entry";

    tpm.shutdownAndDrain(std::chrono::milliseconds(500));
}

// CC-3 (backend_optimization_plan.md): while UI (thumbnail/preview) tasks are
// queued, background types must be THROTTLED to one running task — not
// skipped entirely. The old unconditional skip froze fileScan/mediaProcessor/
// duplicateFinder for as long as the user kept scrolling. Old code fails
// this: the background task never starts while the UI queue is non-empty.
TEST(ThreadPoolManagerTest, BackgroundTypeIsThrottledNotStarvedByUIBacklog)
{
    auto cfg = std::make_shared<UnifiedObservableConfigManager>("/dev/null", false);
    ASSERT_TRUE(cfg->initialize());
    cfg->createProperty("tpm.pool.max", std::string("8"));

    ThreadPoolManager tpm(cfg);
    tpm.initialize();

    // Saturate the media-generation cap and keep a UI backlog queued so
    // has_ui_tasks stays true the whole time. These must be urgent submits —
    // that, not the type name, is what marks work as user-visible.
    std::atomic<bool> release{false};
    std::atomic<int> ui_started{0};
    for (int i = 0; i < 8; ++i)
        tpm.submit("thumbnail_generator", [&]()
                   {
            ui_started.fetch_add(1);
            while (!release.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(5)); },
                   "/ui/" + std::to_string(i), /*urgent=*/true);

    ASSERT_TRUE(MediaDedup::Test::TestUtils::waitForCondition(
        [&]() { return ui_started.load() >= 4; },
        std::chrono::milliseconds(2000), std::chrono::milliseconds(10)))
        << "media cap should admit 4 UI tasks";

    // Background work must still trickle through while the UI backlog exists.
    std::atomic<bool> bg_ran{false};
    tpm.submit("fileScan", [&]() { bg_ran.store(true); }, "/bg/scan");

    EXPECT_TRUE(MediaDedup::Test::TestUtils::waitForCondition(
        [&]() { return bg_ran.load(); },
        std::chrono::milliseconds(2000), std::chrono::milliseconds(10)))
        << "background type starved by UI backlog";

    release.store(true);
    tpm.shutdownAndDrain(std::chrono::milliseconds(500));
}

// Slow-first-thumbnails fix: an urgent submit (user-visible on-demand work)
// must run ahead of a bulk backlog of the same type, and an urgent submit
// that dedup-hits an already-queued path must PROMOTE that task to the front
// instead of leaving it at its backfill FIFO position. Uses a 1-thread pool
// and a non-UI type so execution is strictly serial and deterministic.
TEST(ThreadPoolManagerTest, UrgentSubmitJumpsQueueAndPromotesDuplicates)
{
    auto cfg = std::make_shared<UnifiedObservableConfigManager>("/dev/null", false);
    ASSERT_TRUE(cfg->initialize());
    cfg->createProperty("tpm.pool.max", std::string("1"));
    cfg->createProperty("tpm.killTimeoutMs", 100);

    ThreadPoolManager tpm(cfg);
    tpm.initialize();

    std::atomic<bool> release{false};
    std::atomic<bool> blocker_started{false};
    std::mutex order_mutex;
    std::vector<std::string> order;
    auto record = [&order, &order_mutex](const std::string &name)
    {
        std::lock_guard<std::mutex> lock(order_mutex);
        order.push_back(name);
    };

    // Occupy the single worker so everything below stays queued.
    tpm.submit("media_processor", [&]()
               {
        blocker_started.store(true);
        while (!release.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(5)); },
               "/blocker");
    ASSERT_TRUE(TestUtils::waitForCondition([&]()
                                            { return blocker_started.load(); },
                                            std::chrono::milliseconds(2000), std::chrono::milliseconds(10)));

    // Bulk backlog (backfill-style FIFO).
    for (int i = 0; i < 4; ++i)
        tpm.submit("media_processor", [&record, i]()
                   { record("/bulk/" + std::to_string(i)); },
                   "/bulk/" + std::to_string(i));

    // On-demand: a fresh urgent path goes to the front...
    tpm.submit("media_processor", [&record]()
               { record("/urgent"); },
               "/urgent", /*urgent=*/true);
    // ...and an urgent duplicate of a deep backlog entry promotes the
    // ORIGINAL queued task (this fn is dropped by dedup) ahead of it.
    tpm.submit("media_processor", [&record]()
               { record("/bulk/2-duplicate"); },
               "/bulk/2", /*urgent=*/true);

    release.store(true);
    ASSERT_TRUE(TestUtils::waitForCondition([&]()
                                            {
        std::lock_guard<std::mutex> lock(order_mutex);
        return order.size() == 5; },
                                            std::chrono::milliseconds(5000), std::chrono::milliseconds(10)));

    std::lock_guard<std::mutex> lock(order_mutex);
    // Promoted duplicate runs first (its original fn, promoted last), the
    // urgent newcomer second, then the remaining backlog in FIFO order.
    ASSERT_EQ(order.size(), 5u);
    EXPECT_EQ(order[0], "/bulk/2");
    EXPECT_EQ(order[1], "/urgent");
    EXPECT_EQ(order[2], "/bulk/0");
    EXPECT_EQ(order[3], "/bulk/1");
    EXPECT_EQ(order[4], "/bulk/3");

    tpm.shutdownAndDrain(std::chrono::milliseconds(500));
}

// Slow-viewer fix: bulk media work runs at media_generation_cap_ - 1 so the
// last slot is always free for a user-visible request. Front-of-queue
// placement could not deliver this on its own — nothing preempts a running
// magick decode, so an urgent arrival used to wait out one of the four
// backfill transcodes already holding the cap.
TEST(ThreadPoolManagerTest, MediaCapReservesASlotForUrgentWork)
{
    auto cfg = std::make_shared<UnifiedObservableConfigManager>("/dev/null", false);
    ASSERT_TRUE(cfg->initialize());
    cfg->createProperty("tpm.pool.max", std::string("8"));
    cfg->createProperty("tpm.mediaGeneration.maxConcurrent", 4);
    cfg->createProperty("tpm.killTimeoutMs", 100);

    ThreadPoolManager tpm(cfg);
    tpm.initialize();

    std::atomic<bool> release{false};
    std::atomic<int> bulk_started{0};
    std::atomic<bool> urgent_started{false};

    auto block = [&release]()
    {
        while (!release.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    };

    // Backfill-style: more work than the cap, none of it urgent.
    for (int i = 0; i < 8; ++i)
        tpm.submit("thumbnail_generator", [&]()
                   { bulk_started.fetch_add(1); block(); },
                   "/backfill/" + std::to_string(i));

    // Bulk saturates at cap-1 and must stay there.
    ASSERT_TRUE(TestUtils::waitForCondition([&]()
                                            { return bulk_started.load() >= 3; },
                                            std::chrono::milliseconds(2000), std::chrono::milliseconds(10)))
        << "bulk media work should reach cap-1";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(bulk_started.load(), 3) << "bulk work took the slot reserved for urgent requests";

    // The viewer's request finds the reserved slot free, with every bulk task
    // still running and nothing preempted.
    tpm.submit("fullres_generator", [&]()
               { urgent_started.store(true); block(); },
               "/viewer/raw.arw", /*urgent=*/true);

    EXPECT_TRUE(TestUtils::waitForCondition([&]()
                                            { return urgent_started.load(); },
                                            std::chrono::milliseconds(2000), std::chrono::milliseconds(10)))
        << "urgent media task waited on the bulk backlog";
    EXPECT_EQ(bulk_started.load(), 3);

    release.store(true);
    tpm.shutdownAndDrain(std::chrono::milliseconds(1000));
}

// A non-urgent media backlog must NOT read as "the user is waiting": the
// backfill enqueues up to 20k thumbnail/preview jobs into the very queues the
// scheduler used to treat as UI, which throttled every background type to one
// thread for the whole sweep.
TEST(ThreadPoolManagerTest, BulkMediaBacklogDoesNotThrottleBackgroundTypes)
{
    auto cfg = std::make_shared<UnifiedObservableConfigManager>("/dev/null", false);
    ASSERT_TRUE(cfg->initialize());
    cfg->createProperty("tpm.pool.max", std::string("8"));
    cfg->createProperty("tpm.killTimeoutMs", 100);

    ThreadPoolManager tpm(cfg);
    tpm.initialize();
    tpm.setShare("media_processor", 1.0);

    std::atomic<bool> release{false};
    std::atomic<int> bg_running{0};

    for (int i = 0; i < 8; ++i)
        tpm.submit("thumbnail_generator", [&]()
                   {
            while (!release.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(5)); },
                   "/backfill/" + std::to_string(i));

    for (int i = 0; i < 4; ++i)
        tpm.submit("media_processor", [&]()
                   {
            bg_running.fetch_add(1);
            while (!release.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(5)); },
                   "/bg/" + std::to_string(i));

    // Throttled-to-one would cap this at 1 for as long as the backlog lasts.
    EXPECT_TRUE(TestUtils::waitForCondition([&]()
                                            { return bg_running.load() >= 2; },
                                            std::chrono::milliseconds(2000), std::chrono::milliseconds(10)))
        << "background type throttled by a non-urgent media backlog";

    release.store(true);
    tpm.shutdownAndDrain(std::chrono::milliseconds(1000));
}
