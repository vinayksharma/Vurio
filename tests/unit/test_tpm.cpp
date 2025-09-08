#include <gtest/gtest.h>
#include "orchestration/thread_pool_manager.hpp"
#include "config/unified_observable_config.hpp"
#include <atomic>

using namespace MediaDedup;

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

#if !defined(ALL_UNIT_TESTS)
int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
#endif
