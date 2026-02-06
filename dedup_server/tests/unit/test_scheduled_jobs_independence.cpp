#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <chrono>
#include <memory>

#include "orchestration/scheduler_service.hpp"
#include "orchestration/thread_pool_manager.hpp"
#include "config/unified_observable_config.hpp"

using namespace MediaDedup;
using namespace MediaDedup::Orchestration;

class ScheduledJobsIndependenceTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Create config manager
        config_manager_ = std::make_shared<UnifiedObservableConfigManager>("", false);
        config_manager_->initialize();

        // Create thread pool manager
        tpm_ = std::make_shared<ThreadPoolManager>(config_manager_);
        tpm_->initialize();

        // Create scheduler service
        scheduler_ = std::make_shared<SchedulerService>(config_manager_, tpm_);
        scheduler_->start();

        // Reset counters
        scheduled_executions_ = 0;
        on_demand_executions_ = 0;
    }

    void TearDown() override
    {
        if (scheduler_)
        {
            scheduler_->stop();
        }
        if (tpm_)
        {
            tpm_->shutdownAndDrain(std::chrono::milliseconds(1000));
        }
    }

    std::shared_ptr<UnifiedObservableConfigManager> config_manager_;
    std::shared_ptr<ThreadPoolManager> tpm_;
    std::shared_ptr<SchedulerService> scheduler_;

    std::atomic<int> scheduled_executions_{0};
    std::atomic<int> on_demand_executions_{0};
};

TEST_F(ScheduledJobsIndependenceTest, ScheduledJobsContinueAfterOnDemandTrigger)
{
    // Register a scheduled job that runs every 1 second
    scheduler_->registerJob("testJob", std::chrono::milliseconds(1000), "test", [this]()
                            {
        scheduled_executions_.fetch_add(1);
        std::cout << "Scheduled job executed (count: " << scheduled_executions_.load() << ")" << std::endl; });

    // Wait for initial scheduled execution
    auto start = std::chrono::steady_clock::now();
    while (scheduled_executions_.load() < 1 &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(5))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    EXPECT_GE(scheduled_executions_.load(), 1) << "Scheduled job should have executed at least once";

    // Trigger on-demand execution
    bool triggered = scheduler_->triggerJob("testJob");
    EXPECT_TRUE(triggered) << "On-demand trigger should succeed";

    // Wait for more scheduled executions
    int initial_count = scheduled_executions_.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(1500)); // Wait 1.5 seconds
    int final_count = scheduled_executions_.load();

    // Should have at least one more scheduled execution
    EXPECT_GT(final_count, initial_count) << "Scheduled jobs should continue running after on-demand trigger";

    std::cout << "Scheduled executions before on-demand: " << initial_count << std::endl;
    std::cout << "Scheduled executions after on-demand: " << final_count << std::endl;
}

TEST_F(ScheduledJobsIndependenceTest, OnDemandTriggerDoesNotAffectScheduledTiming)
{
    // Register a scheduled job with a specific interval
    auto job_start_time = std::chrono::steady_clock::now();
    scheduler_->registerJob("timingTestJob", std::chrono::milliseconds(2000), "test", [this, job_start_time]()
                            {
        auto execution_time = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(execution_time - job_start_time);
        scheduled_executions_.fetch_add(1);
        std::cout << "Scheduled job executed at " << elapsed.count() << "ms" << std::endl; });

    // Wait for first execution
    auto start = std::chrono::steady_clock::now();
    while (scheduled_executions_.load() < 1 &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(5))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    EXPECT_GE(scheduled_executions_.load(), 1) << "First scheduled execution should occur";

    // Trigger on-demand execution
    bool triggered = scheduler_->triggerJob("timingTestJob");
    EXPECT_TRUE(triggered) << "On-demand trigger should succeed";

    // Wait for next scheduled execution (should be around 2 seconds after first)
    int initial_count = scheduled_executions_.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(2500)); // Wait 2.5 seconds
    int final_count = scheduled_executions_.load();

    // Should have at least one more scheduled execution
    EXPECT_GT(final_count, initial_count) << "Scheduled jobs should continue on their regular schedule";
}
