#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <atomic>
#include "orchestration/scheduler_service.hpp"
#include "config/unified_observable_config.hpp"
#include "orchestration/thread_pool_manager.hpp"

using namespace MediaDedup::Orchestration;
using namespace MediaDedup;

class SchedulerConcurrencyTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        config_manager_ = std::make_shared<UnifiedObservableConfigManager>("config/config.yaml");
        tpm_ = std::make_shared<ThreadPoolManager>(config_manager_);
        scheduler_ = std::make_shared<SchedulerService>(config_manager_, tpm_);
        scheduler_->start();
    }

    void TearDown() override
    {
        if (scheduler_)
        {
            scheduler_->stop();
        }
    }

    std::shared_ptr<UnifiedObservableConfigManager> config_manager_;
    std::shared_ptr<ThreadPoolManager> tpm_;
    std::shared_ptr<SchedulerService> scheduler_;
};

TEST_F(SchedulerConcurrencyTest, TriggerJob_WhenNotRunning_ExecutesSuccessfully)
{
    std::atomic<int> execution_count{0};
    std::atomic<bool> job_completed{false};

    // Register a test job
    scheduler_->registerJob("testJob", std::chrono::milliseconds(1000), "test", [&]() {
        execution_count.fetch_add(1);
        job_completed.store(true);
    });

    // Trigger the job on-demand
    bool success = scheduler_->triggerJob("testJob");
    EXPECT_TRUE(success);

    // Wait for job to complete
    auto start = std::chrono::steady_clock::now();
    while (!job_completed.load() && 
           std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_TRUE(job_completed.load());
    EXPECT_EQ(execution_count.load(), 1);
}

TEST_F(SchedulerConcurrencyTest, TriggerJob_WhenAlreadyRunning_SkipsExecution)
{
    std::atomic<int> execution_count{0};
    std::atomic<bool> job_started{false};
    std::atomic<bool> job_completed{false};

    // Register a long-running test job
    scheduler_->registerJob("longJob", std::chrono::milliseconds(1000), "test", [&]() {
        job_started.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(500)); // Long running
        execution_count.fetch_add(1);
        job_completed.store(true);
    });

    // Trigger the job first time
    bool success1 = scheduler_->triggerJob("longJob");
    EXPECT_TRUE(success1);

    // Wait for job to start
    auto start = std::chrono::steady_clock::now();
    while (!job_started.load() && 
           std::chrono::steady_clock::now() - start < std::chrono::seconds(2)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(job_started.load());

    // Try to trigger again while running - should be skipped
    bool success2 = scheduler_->triggerJob("longJob");
    EXPECT_FALSE(success2); // Should be skipped

    // Wait for job to complete
    start = std::chrono::steady_clock::now();
    while (!job_completed.load() && 
           std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_TRUE(job_completed.load());
    EXPECT_EQ(execution_count.load(), 1); // Should only execute once
}

TEST_F(SchedulerConcurrencyTest, IsJobRunning_ReturnsCorrectState)
{
    std::atomic<bool> job_started{false};
    std::atomic<bool> job_completed{false};

    // Register a test job
    scheduler_->registerJob("stateJob", std::chrono::milliseconds(1000), "test", [&]() {
        job_started.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        job_completed.store(true);
    });

    // Initially not running
    EXPECT_FALSE(scheduler_->isJobRunning("stateJob"));

    // Trigger the job
    scheduler_->triggerJob("stateJob");

    // Wait for job to start
    auto start = std::chrono::steady_clock::now();
    while (!job_started.load() && 
           std::chrono::steady_clock::now() - start < std::chrono::seconds(2)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Should be running now
    EXPECT_TRUE(scheduler_->isJobRunning("stateJob"));

    // Wait for job to complete
    start = std::chrono::steady_clock::now();
    while (!job_completed.load() && 
           std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Should not be running anymore
    EXPECT_FALSE(scheduler_->isJobRunning("stateJob"));
}

TEST_F(SchedulerConcurrencyTest, GetJobStatuses_ReturnsCorrectInformation)
{
    // Register a test job
    scheduler_->registerJob("statusJob", std::chrono::milliseconds(2000), "test", [&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    });

    auto statuses = scheduler_->getJobStatuses();
    EXPECT_EQ(statuses.size(), 1);
    
    const auto& status = statuses[0];
    EXPECT_EQ(status.jobId, "statusJob");
    EXPECT_EQ(status.interval.count(), 2000);
    EXPECT_EQ(status.consecutiveFailures, 0);
    EXPECT_EQ(status.totalFailures, 0);
}

TEST_F(SchedulerConcurrencyTest, ListJobs_ReturnsRegisteredJobs)
{
    // Register multiple jobs
    scheduler_->registerJob("job1", std::chrono::milliseconds(1000), "test", []() {});
    scheduler_->registerJob("job2", std::chrono::milliseconds(2000), "test", []() {});

    auto jobs = scheduler_->listJobs();
    EXPECT_EQ(jobs.size(), 2);
    
    // Check that both jobs are in the list
    EXPECT_TRUE(std::find(jobs.begin(), jobs.end(), "job1") != jobs.end());
    EXPECT_TRUE(std::find(jobs.begin(), jobs.end(), "job2") != jobs.end());
}
