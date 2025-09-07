#include <gtest/gtest.h>
#include "core/console_input_manager.hpp"
#include <atomic>
#include <chrono>
#include <thread>

using MediaDedupServer::Core::ConsoleEvent;
using MediaDedupServer::Core::ConsoleEventType;
using MediaDedupServer::Core::ConsoleInputManager;

TEST(ConsoleInputManagerTest, ProcessesHelpCommand)
{
    auto &mgr = ConsoleInputManager::getInstance();
    ASSERT_TRUE(mgr.initialize());

    std::atomic<bool> received{false};
    auto subId = mgr.subscribeToConsoleEvents([&](const ConsoleEvent &evt)
                                              {
        if (evt.type == ConsoleEventType::COMMAND_HELP)
            received = true; });

    mgr.processCommand("help");

    // Small wait for callback dispatch
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_TRUE(received.load());

    mgr.unsubscribeFromConsoleEvents(subId);
    mgr.shutdown();
}

TEST(ConsoleInputManagerTest, ProcessesExitCommand)
{
    auto &mgr = ConsoleInputManager::getInstance();
    ASSERT_TRUE(mgr.initialize());

    std::atomic<bool> received{false};
    auto subId = mgr.subscribeToConsoleEvents([&](const ConsoleEvent &evt)
                                              {
        if (evt.type == ConsoleEventType::COMMAND_EXIT)
            received = true; });

    mgr.processCommand("exit");
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_TRUE(received.load());

    mgr.unsubscribeFromConsoleEvents(subId);
    mgr.shutdown();
}

#if !defined(ALL_UNIT_TESTS)
// Provide a test main for this standalone test binary
int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
#endif
