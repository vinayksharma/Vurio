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
        running_.store(true, std::memory_order_relaxed);
    }

    void SchedulerService::stop()
    {
        running_.store(false, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(jobsMutex_);
        for (auto &kv : jobs_)
        {
            auto &job = *kv.second;
            job.stopFlag.store(true, std::memory_order_relaxed);
        }
        for (auto &kv : jobs_)
        {
            auto &job = *kv.second;
            if (job.worker.joinable())
                job.worker.join();
        }
        jobs_.clear();
    }

    void SchedulerService::registerJob(const std::string &jobId,
                                       milliseconds interval,
                                       const std::string &typeKey,
                                       std::function<void()> callback)
    {
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
        // Dynamic interval per job (if key exists): scheduler.jobs.<jobId>.intervalMs
        std::string key = std::string("scheduler.jobs.") + job.jobId + ".intervalMs";
        int intervalMs = cfg_->getPropertyValue<int>(key, static_cast<int>(job.interval.count()));
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
