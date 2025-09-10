#include "orchestration/scheduler_service.hpp"
#include <random>
#include <Poco/Logger.h>

namespace MediaDedup::Orchestration
{
    using namespace std::chrono;

    SchedulerService::SchedulerService(std::shared_ptr<UnifiedObservableConfigManager> cfg,
                                       std::shared_ptr<ThreadPoolManager> tpm)
        : cfg_(std::move(cfg)), tpm_(std::move(tpm)) {}

    SchedulerService::~SchedulerService()
    {
        stop();
    }

    void SchedulerService::start()
    {
        Poco::Logger &logger = Poco::Logger::get("SchedulerService");
        logger.information("Starting SchedulerService...");

        running_.store(true, std::memory_order_relaxed);

        logger.information("SchedulerService started successfully");
    }

    void SchedulerService::stop()
    {
        Poco::Logger &logger = Poco::Logger::get("SchedulerService");
        logger.information("Stopping SchedulerService...");

        running_.store(false, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(jobsMutex_);

        logger.debug("Stopping %zu registered jobs", jobs_.size());
        for (auto &kv : jobs_)
        {
            auto &job = *kv.second;
            job.stopFlag.store(true, std::memory_order_relaxed);
            logger.debug("Stopped job: %s", kv.first);
        }
        for (auto &kv : jobs_)
        {
            auto &job = *kv.second;
            if (job.worker.joinable())
            {
                logger.debug("Joining worker thread for job: %s", kv.first);
                job.worker.join();
            }
        }
        jobs_.clear();

        logger.information("SchedulerService stopped successfully");
    }

    void SchedulerService::registerJob(const std::string &jobId,
                                       milliseconds interval,
                                       const std::string &typeKey,
                                       std::function<void()> callback)
    {
        Poco::Logger &logger = Poco::Logger::get("SchedulerService");
        logger.information("Registering job: %s (type: %s, interval: %ld ms)",
                           jobId, typeKey, static_cast<long>(interval.count()));

        auto job = std::make_unique<Job>();
        job->jobId = jobId;
        job->typeKey = typeKey;
        job->callback = std::move(callback);
        job->interval = interval;
        job->backoff = milliseconds(0);

        {
            std::lock_guard<std::mutex> lock(jobsMutex_);
            jobs_[jobId] = std::move(job);
        }

        // Launch worker thread
        {
            std::lock_guard<std::mutex> lock(jobsMutex_);
            Job &ref = *jobs_[jobId];
            logger.debug("Starting worker thread for job: %s", jobId);
            ref.worker = std::thread([this, &ref]
                                     {
                                         std::mt19937 rng{std::random_device{}()};
                                         while (running_.load(std::memory_order_relaxed) && !ref.stopFlag.load(std::memory_order_relaxed))
                                         {
                                             refreshJobConfig(ref);

                                             milliseconds base = ref.interval;
                                             milliseconds delay = computeNextDelay(ref, base);

                                             // sleep respecting stop flag
                                             auto slept = milliseconds(0);
                                             const auto step = milliseconds(50);
                                             while (slept < delay && running_.load(std::memory_order_relaxed) && !ref.stopFlag.load(std::memory_order_relaxed))
                                             {
                                                 std::this_thread::sleep_for(step);
                                                 slept += step;
                                             }
                                             if (!running_.load(std::memory_order_relaxed) || ref.stopFlag.load(std::memory_order_relaxed)) break;

                                             // submit job
                                            try
                                            {
                                                // Info log that a new job dispatch is starting
                                                {
                                                    Poco::Logger &logger = Poco::Logger::get("SchedulerService");
                                                    logger.information(std::string("Dispatching job: ") + ref.jobId +
                                                                       ", typeKey=" + ref.typeKey +
                                                                       ", intervalMs=" + std::to_string(ref.interval.count()));
                                                    logger.trace("Submitting job '" + ref.jobId + "' to ThreadPoolManager for execution");
                                                }
                                                tpm_->submit(ref.typeKey, [cb = ref.callback]() { cb(); });
                                                // reset backoff on success path (submission OK)
                                                ref.backoff = milliseconds(0);
                                            }
                                             catch (...)
                                             {
                                                 // submission failed, apply backoff
                                                 if (cfg_->getPropertyValue<bool>("scheduler.backoff.enabled", true))
                                                 {
                                                     milliseconds initial(cfg_->getPropertyValue<int>("scheduler.backoff.initialMs", 1000));
                                                     milliseconds maxB(cfg_->getPropertyValue<int>("scheduler.backoff.maxMs", 30000));
                                                     double mult = 2.0;
                                                     if (auto prop = cfg_->getProperty<double>("scheduler.backoff.multiplier"))
                                                     {
                                                         mult = std::any_cast<double>(prop->getValue());
                                                     }
                                                     if (ref.backoff.count() == 0) ref.backoff = initial; else ref.backoff = milliseconds(std::min<long long>(maxB.count(), static_cast<long long>(ref.backoff.count() * mult)));
                                                 }
                                             }
                                         } });
        }
    }

    void SchedulerService::unregisterJob(const std::string &jobId)
    {
        std::unique_ptr<Job> to_join;
        {
            std::lock_guard<std::mutex> lock(jobsMutex_);
            auto it = jobs_.find(jobId);
            if (it == jobs_.end())
                return;
            it->second->stopFlag.store(true, std::memory_order_relaxed);
            to_join = std::move(it->second);
            jobs_.erase(it);
        }
        if (to_join && to_join->worker.joinable())
            to_join->worker.join();
    }

    void SchedulerService::refreshJobConfig(Job &job)
    {
        int intervalMs = static_cast<int>(job.interval.count());

        // Check for job-specific configuration first: scheduler.jobs.<jobId>.intervalMs
        std::string jobKey = std::string("scheduler.jobs.") + job.jobId + ".intervalMs";
        if (cfg_->hasProperty(jobKey))
        {
            intervalMs = cfg_->getPropertyValue<int>(jobKey, intervalMs);
        }
        // Special case for fileScan job: also check files.manager.scan.intervalMs
        else if (job.jobId == "fileScan")
        {
            int originalInterval = intervalMs;
            intervalMs = cfg_->getPropertyValue<int>("files.manager.scan.intervalMs", intervalMs);
            Poco::Logger &logger = Poco::Logger::get("SchedulerService");
            logger.debug("fileScan job config refresh: original=%d, config value=%d", originalInterval, intervalMs);
        }

        if (intervalMs != static_cast<int>(job.interval.count()))
        {
            Poco::Logger &logger = Poco::Logger::get("SchedulerService");
            logger.information("Job '%s' interval updated: %ld ms -> %d ms",
                               job.jobId, job.interval.count(), intervalMs);
        }

        job.interval = milliseconds(intervalMs);
    }

    std::chrono::milliseconds SchedulerService::computeNextDelay(const Job &job, std::chrono::milliseconds baseInterval)
    {
        using std::min;
        auto interval = baseInterval;

        // backoff
        if (job.backoff.count() > 0)
        {
            interval = min(interval + job.backoff, interval * 2);
        }

        // jitter
        bool jitterEnabled = cfg_->getPropertyValue<bool>("scheduler.jitter.enabled", false);
        int jitterPercent = cfg_->getPropertyValue<int>("scheduler.jitter.percent", 0);
        // per-job overrides
        std::string jp = std::string("scheduler.jobs.") + job.jobId + ".jitter.percent";
        if (cfg_->hasProperty(jp))
        {
            jitterPercent = cfg_->getPropertyValue<int>(jp, jitterPercent);
            jitterEnabled = true;
        }
        if (jitterEnabled && jitterPercent > 0)
        {
            long long span = interval.count() * jitterPercent / 100;
            long long minDelay = interval.count() - span;
            long long maxDelay = interval.count() + span;
            static thread_local std::mt19937 rng{std::random_device{}()};
            std::uniform_int_distribution<long long> dist(minDelay, maxDelay);
            interval = milliseconds(dist(rng));
        }

        // drift control (simplified anchor: no accumulated state here; future extension can store anchors)
        // we rely on fixedDelay semantics; anchored mode to be handled by an anchor schedule in future revisions

        return interval;
    }
}
