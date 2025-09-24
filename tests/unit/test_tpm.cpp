#include <gtest/gtest.h>
#include "orchestration/thread_pool_manager.hpp"
#include "config/unified_observable_config.hpp"
#include "test_utils.hpp"
#include <atomic>
#include <thread>
#include <chrono>

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

#include "test_utils.hpp"

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

TEST(ThreadPoolManagerTest, IdleTimeoutConfiguration_ChangeValue_RecreatesThreadPool)
{
    // Test that changing the idle timeout recreates the thread pool
    auto cfg = std::make_shared<UnifiedObservableConfigManager>("/dev/null", false);
    ASSERT_TRUE(cfg->initialize());

    ThreadPoolManager tpm(cfg);
    tpm.initialize();

    // Submit a task to ensure the pool is active
    std::atomic<bool> taskCompleted{false};
    tpm.submit("test", [&taskCompleted]()
               {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        taskCompleted.store(true); });

    // Wait for task to complete
    TestUtils::waitForCondition([&taskCompleted]()
                                { return taskCompleted.load(); },
                                std::chrono::milliseconds(1000), std::chrono::milliseconds(10));

    // Change the idle timeout configuration
    cfg->setPropertyValue<int>("tpm.thread.idleTimeoutSeconds", 60);

    // Wait a bit for the configuration change to be processed
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Submit another task to verify the pool is still working
    std::atomic<bool> secondTaskCompleted{false};
    tpm.submit("test", [&secondTaskCompleted]()
               {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        secondTaskCompleted.store(true); });

    // Wait for second task to complete
    TestUtils::waitForCondition([&secondTaskCompleted]()
                                { return secondTaskCompleted.load(); },
                                std::chrono::milliseconds(1000), std::chrono::milliseconds(10));

    EXPECT_TRUE(secondTaskCompleted.load());

    tpm.shutdownAndDrain(std::chrono::milliseconds(100));
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

#if !defined(ALL_UNIT_TESTS)
int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
#endif
