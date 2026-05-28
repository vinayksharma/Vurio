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
        tpm_->initialize(); // Initialize the thread pool manager
        scheduler_ = std::make_shared<SchedulerService>(config_manager_, tpm_);
        scheduler_->start();
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
};

TEST_F(SchedulerConcurrencyTest, TriggerJob_WhenNotRunning_ExecutesSuccessfully)
{
    std::atomic<int> execution_count{0};
    std::atomic<bool> job_completed{false};

    // Register a test job
    scheduler_->registerJob("testJob", std::chrono::milliseconds(1000), "test", [&]()
                            {
        std::cout << "Job callback executing!" << std::endl;
        execution_count.fetch_add(1);
        job_completed.store(true); 
        std::cout << "Job callback completed!" << std::endl; });

    // Trigger the job on-demand
    bool success = scheduler_->triggerJob("testJob");
    EXPECT_TRUE(success);

    std::cout << "Job triggered, waiting for completion..." << std::endl;

    // Wait for job to complete
    auto start = std::chrono::steady_clock::now();
    while (!job_completed.load() &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(5))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::cout << "Final state - completed: " << job_completed.load()
              << ", count: " << execution_count.load() << std::endl;

    EXPECT_TRUE(job_completed.load());
    EXPECT_EQ(execution_count.load(), 1);
}

TEST_F(SchedulerConcurrencyTest, TriggerJob_WhenAlreadyRunning_SkipsExecution)
{
    std::atomic<int> execution_count{0};
    std::atomic<bool> job_started{false};
    std::atomic<bool> job_completed{false};

    // Register a long-running test job
    scheduler_->registerJob("longJob", std::chrono::milliseconds(1000), "test", [&]()
                            {
        job_started.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(500)); // Long running
        execution_count.fetch_add(1);
        job_completed.store(true); });

    // Trigger the job first time
    bool success1 = scheduler_->triggerJob("longJob");
    EXPECT_TRUE(success1);

    // Wait for job to start
    auto start = std::chrono::steady_clock::now();
    while (!job_started.load() &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(2))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(job_started.load());

    // Try to trigger again while running - should be skipped
    bool success2 = scheduler_->triggerJob("longJob");
    EXPECT_FALSE(success2); // Should be skipped

    // Wait for job to complete
    start = std::chrono::steady_clock::now();
    while (!job_completed.load() &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(5))
    {
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
    scheduler_->registerJob("stateJob", std::chrono::milliseconds(1000), "test", [&]()
                            {
        job_started.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        job_completed.store(true); });

    // Initially not running
    EXPECT_FALSE(scheduler_->isJobRunning("stateJob"));

    // Trigger the job
    scheduler_->triggerJob("stateJob");

    // Wait for job to start
    auto start = std::chrono::steady_clock::now();
    while (!job_started.load() &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(2))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Should be running now
    EXPECT_TRUE(scheduler_->isJobRunning("stateJob"));

    // Wait for job to complete
    start = std::chrono::steady_clock::now();
    while (!job_completed.load() &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(5))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Should not be running anymore
    EXPECT_FALSE(scheduler_->isJobRunning("stateJob"));
}

TEST_F(SchedulerConcurrencyTest, GetJobStatuses_ReturnsCorrectInformation)
{
    // Register a test job
    scheduler_->registerJob("statusJob", std::chrono::milliseconds(2000), "test", [&]()
                            { std::this_thread::sleep_for(std::chrono::milliseconds(100)); });

    auto statuses = scheduler_->getJobStatuses();
    EXPECT_EQ(statuses.size(), 1);

    const auto &status = statuses[0];
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

TEST_F(SchedulerConcurrencyTest, ScheduledDispatch_WhenAlreadyRunning_SkipsDispatch)
{
    std::atomic<int> execution_count{0};
    std::atomic<bool> job_started{false};
    std::atomic<bool> release_job{false};

    // Register a test job with a very short interval (50 ms) to trigger rapid scheduled dispatches.
    // The callback blocks until release_job is true.
    scheduler_->registerJob("rapidJob", std::chrono::milliseconds(50), "test", [&]()
                            {
        job_started.store(true);
        execution_count.fetch_add(1);
        while (!release_job.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    // Wait until the job starts the first time.
    auto start = std::chrono::steady_clock::now();
    while (!job_started.load() &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(3))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(job_started.load());

    // Wait for a few intervals (e.g. 250 ms) while the job is still running/blocking.
    // If the check works, scheduled dispatches during this time will be skipped,
    // and the execution count will remain exactly 1.
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    // Verify that it only executed exactly once (no duplicate queueing occurred)
    EXPECT_EQ(execution_count.load(), 1);

    // Release the job so it can complete.
    release_job.store(true);

    // Wait a bit to ensure it completes
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

