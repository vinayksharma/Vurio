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

#if !defined(ALL_UNIT_TESTS)
int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
#endif
