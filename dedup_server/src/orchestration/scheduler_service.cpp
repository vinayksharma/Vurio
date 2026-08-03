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
        paused_.store(false, std::memory_order_relaxed);
        if (tpm_) {
            tpm_->resume();
        }

        logger.information("SchedulerService started successfully");
    }

    void SchedulerService::stop()
    {
        Poco::Logger &logger = Poco::Logger::get("SchedulerService");
        logger.information("Stopping SchedulerService...");

        running_.store(false, std::memory_order_relaxed);

        // CC-4: move the jobs out under the lock, join OUTSIDE it. The old
        // code joined workers while holding jobsMutex_, which the TPM job
        // lambdas also take to record completion — a fragile non-deadlock
        // that depended on the worker exit path never touching the mutex.
        std::vector<std::unique_ptr<Job>> to_join;
        {
            std::lock_guard<std::mutex> lock(jobsMutex_);
            logger.debug("Stopping %u registered jobs", static_cast<unsigned int>(jobs_.size()));
            for (auto &kv : jobs_)
            {
                kv.second->stopFlag.store(true, std::memory_order_relaxed);
                logger.debug("Stopped job: %s", kv.first);
            }
            to_join.reserve(jobs_.size());
            for (auto &kv : jobs_)
                to_join.push_back(std::move(kv.second));
            jobs_.clear();
        }
        wakeCv_.notify_all();
        for (auto &job : to_join)
        {
            if (job->worker.joinable())
            {
                logger.debug("Joining worker thread for job: %s", job->jobId);
                job->worker.join();
            }
        }

        logger.information("SchedulerService stopped successfully");
    }

    void SchedulerService::pause()
    {
        Poco::Logger &logger = Poco::Logger::get("SchedulerService");
        logger.information("Pausing SchedulerService...");
        paused_.store(true, std::memory_order_relaxed);
        if (tpm_) {
            tpm_->pause();
        }
        // CC-4: copy under the mutex, invoke outside it.
        std::function<void()> cb;
        {
            std::lock_guard<std::mutex> lock(callbackMutex_);
            cb = pauseCallback_;
        }
        if (cb) {
            cb();
        }
    }

    void SchedulerService::resume()
    {
        Poco::Logger &logger = Poco::Logger::get("SchedulerService");
        logger.information("Resuming SchedulerService...");
        paused_.store(false, std::memory_order_relaxed);
        if (tpm_) {
            tpm_->resume();
        }
        wakeCv_.notify_all();
    }

    bool SchedulerService::isPaused() const
    {
        return paused_.load(std::memory_order_relaxed);
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
        job->interval_ms.store(interval.count());
        job->backoff_ms.store(0);

        // Load per-job failure limit at registration time
        std::string configKey = "scheduler.jobs." + jobId + ".maxConsecutiveFailures";
        job->maxConsecutiveFailures = cfg_->getPropertyValue<int>(configKey, 5);
        logger.debug("Job %s max consecutive failures: %d", jobId, job->maxConsecutiveFailures);

        std::lock_guard<std::mutex> lock(jobsMutex_);

        // CC-4: overwriting an existing entry destroyed a live Job whose
        // worker thread still held a pointer into it (use-after-free, then
        // std::terminate from the joinable thread's destructor).
        if (jobs_.count(jobId) != 0)
        {
            logger.error("Job '%s' is already registered — ignoring duplicate registration", jobId);
            return;
        }

        Job &ref = *(jobs_[jobId] = std::move(job));
        logger.debug("Starting worker thread for job: %s", jobId);
        Job *jobPtr = &ref;
        ref.worker = std::thread([this, jobPtr]
                                 {
            Job &ref = *jobPtr;
            while (running_.load(std::memory_order_relaxed) && !ref.stopFlag.load(std::memory_order_relaxed))
            {
                refreshJobConfig(ref);

                milliseconds base(ref.interval_ms.load(std::memory_order_relaxed));
                milliseconds delay = computeNextDelay(ref, base);

                // CC-4: block on the CV instead of 50 ms sleep-polling; stop
                // and unregister wake us immediately.
                {
                    std::unique_lock<std::mutex> lk(wakeMutex_);
                    wakeCv_.wait_for(lk, delay, [&]
                                     { return !running_.load(std::memory_order_relaxed) ||
                                              ref.stopFlag.load(std::memory_order_relaxed); });
                }
                if (!running_.load(std::memory_order_relaxed) || ref.stopFlag.load(std::memory_order_relaxed))
                    break;

                // Wait while paused (resume/stop notify).
                {
                    std::unique_lock<std::mutex> lk(wakeMutex_);
                    wakeCv_.wait(lk, [&]
                                 { return !paused_.load(std::memory_order_relaxed) ||
                                          !running_.load(std::memory_order_relaxed) ||
                                          ref.stopFlag.load(std::memory_order_relaxed); });
                }
                if (!running_.load(std::memory_order_relaxed) || ref.stopFlag.load(std::memory_order_relaxed))
                    break;

                try
                {
                    if (!executeJobIfIdle(ref, "scheduled"))
                    {
                        Poco::Logger::get("SchedulerService").debug("Job %s is already running, skipping scheduled dispatch", ref.jobId);
                        continue;
                    }
                    Poco::Logger &logger = Poco::Logger::get("SchedulerService");
                    logger.information(std::string("Dispatched job: ") + ref.jobId +
                                       ", typeKey=" + ref.typeKey +
                                       ", intervalMs=" + std::to_string(ref.interval_ms.load()));
                    // reset backoff on success path (submission OK)
                    ref.backoff_ms.store(0);
                }
                catch (...)
                {
                    // submission failed, apply backoff
                    if (cfg_->getPropertyValue<bool>("scheduler.backoff.enabled", true))
                    {
                        long long initial = cfg_->getPropertyValue<int>("scheduler.backoff.initialMs", 1000);
                        long long maxB = cfg_->getPropertyValue<int>("scheduler.backoff.maxMs", 30000);
                        double mult = 2.0;
                        if (auto prop = cfg_->getProperty<double>("scheduler.backoff.multiplier"))
                        {
                            mult = std::any_cast<double>(prop->getValue());
                        }
                        long long current = ref.backoff_ms.load();
                        ref.backoff_ms.store(current == 0
                                                 ? initial
                                                 : std::min<long long>(maxB, static_cast<long long>(current * mult)));
                    }
                }
            } });
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
        wakeCv_.notify_all();
        if (to_join && to_join->worker.joinable())
            to_join->worker.join();
    }

    bool SchedulerService::updateJobInterval(const std::string &jobId, std::chrono::milliseconds newInterval)
    {
        Poco::Logger &logger = Poco::Logger::get("SchedulerService");

        std::lock_guard<std::mutex> lock(jobsMutex_);
        auto it = jobs_.find(jobId);
        if (it == jobs_.end())
        {
            logger.warning("Cannot update interval for job '%s': job not found", jobId);
            return false;
        }

        auto &job = *it->second;
        long long oldMs = job.interval_ms.load();

        if (oldMs == newInterval.count())
        {
            logger.debug("Job '%s' interval unchanged: %ld ms", jobId, static_cast<long>(oldMs));
            return true;
        }

        job.interval_ms.store(newInterval.count());
        logger.information("Job '%s' interval updated: %ld ms -> %ld ms",
                           jobId, static_cast<long>(oldMs), static_cast<long>(newInterval.count()));

        return true;
    }

    void SchedulerService::refreshJobConfig(Job &job)
    {
        int intervalMs = static_cast<int>(job.interval_ms.load());

        // Check for job-specific configuration first: scheduler.jobs.<jobId>.intervalMs
        std::string jobKey = std::string("scheduler.jobs.") + job.jobId + ".intervalMs";
        if (cfg_->hasProperty(jobKey))
        {
            intervalMs = cfg_->getPropertyValue<int>(jobKey, intervalMs);
        }
        // Special case for fileScan job: also check files.manager.scan.intervalMs
        else if (job.jobId == "fileScan")
        {
            intervalMs = cfg_->getPropertyValue<int>("files.manager.scan.intervalMs", intervalMs);
        }
        // Special case for mediaProcessor job: also check media.processor.intervalMs
        else if (job.jobId == "mediaProcessor")
        {
            intervalMs = cfg_->getPropertyValue<int>("media.processor.intervalMs", intervalMs);
        }
        // Special case for duplicateFinder job: also check duplicates.finder.intervalMs
        else if (job.jobId == "duplicateFinder")
        {
            intervalMs = cfg_->getPropertyValue<int>("duplicates.finder.intervalMs", intervalMs);
        }

        if (intervalMs != static_cast<int>(job.interval_ms.load()))
        {
            Poco::Logger &logger = Poco::Logger::get("SchedulerService");
            logger.information("Job '%s' interval updated: %ld ms -> %d ms",
                               job.jobId, static_cast<long>(job.interval_ms.load()), intervalMs);
        }

        job.interval_ms.store(intervalMs);
    }

    std::chrono::milliseconds SchedulerService::computeNextDelay(const Job &job, std::chrono::milliseconds baseInterval)
    {
        using std::min;
        auto interval = baseInterval;

        // backoff
        long long backoff = job.backoff_ms.load(std::memory_order_relaxed);
        if (backoff > 0)
        {
            interval = min(interval + milliseconds(backoff), interval * 2);
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

        return interval;
    }

    // On-demand job execution
    bool SchedulerService::triggerJob(const std::string &jobId)
    {
        Poco::Logger &logger = Poco::Logger::get("SchedulerService");
        std::lock_guard<std::mutex> lock(jobsMutex_);

        auto it = jobs_.find(jobId);
        if (it == jobs_.end())
        {
            logger.error("Job not found: %s", jobId);
            return false;
        }

        // CC-4: the IDLE->RUNNING claim happens atomically inside
        // executeJobIfIdle — no check-then-act window against the worker.
        if (!executeJobIfIdle(*it->second, "on-demand"))
        {
            logger.debug("Job %s is already running, skipping on-demand trigger", jobId);
            return false; // Not an error, just skipped
        }
        logger.debug("Job %s triggered on-demand", jobId);
        return true;
    }

    bool SchedulerService::isJobRunning(const std::string &jobId)
    {
        std::lock_guard<std::mutex> lock(jobsMutex_);
        auto it = jobs_.find(jobId);
        if (it == jobs_.end())
        {
            return false;
        }
        return it->second->state.load() == JobState::RUNNING;
    }

    bool SchedulerService::isRunning() const
    {
        return running_.load(std::memory_order_relaxed);
    }

    std::vector<std::string> SchedulerService::listJobs()
    {
        std::lock_guard<std::mutex> lock(jobsMutex_);
        std::vector<std::string> jobIds;
        for (const auto &kv : jobs_)
        {
            jobIds.push_back(kv.first);
        }
        return jobIds;
    }

    std::vector<SchedulerService::JobStatus> SchedulerService::getJobStatuses()
    {
        std::lock_guard<std::mutex> lock(jobsMutex_);
        std::vector<JobStatus> statuses;

        for (const auto &kv : jobs_)
        {
            const Job &job = *kv.second;
            JobStatus status;
            status.jobId = job.jobId;
            status.state = job.state.load();
            status.interval = milliseconds(job.interval_ms.load());
            status.lastRun = job.lastRun;
            status.nextRun = job.nextRun;
            status.consecutiveFailures = job.consecutiveFailures.load();
            status.totalFailures = job.totalFailures.load();
            statuses.push_back(status);
        }

        return statuses;
    }

    void SchedulerService::setShutdownCallback(std::function<void()> callback)
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        shutdownCallback_ = std::move(callback);
    }

    void SchedulerService::setPauseCallback(std::function<void()> callback)
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        pauseCallback_ = std::move(callback);
    }

    bool SchedulerService::executeJobIfIdle(Job &job, const std::string &triggerType)
    {
        // CC-4: atomic claim — two dispatchers (worker loop + on-demand
        // trigger) can never both pass a check-then-act gate again.
        JobState expected = JobState::IDLE;
        if (!job.state.compare_exchange_strong(expected, JobState::RUNNING))
            return false;

        // Capture job ID and callback by value to avoid reference issues.
        std::string jobId = job.jobId;
        auto callback = job.callback;

        // CC-4: the TPM lambda may outlive this object through the shutdown
        // drain window — capture a weak_ptr, never raw `this`.
        std::weak_ptr<SchedulerService> weak_self = weak_from_this();

        try
        {
            tpm_->submit(job.typeKey, [weak_self, jobId, callback, triggerType]()
                         {
                Poco::Logger &logger = Poco::Logger::get("SchedulerService");
                bool ok = true;
                std::string error;
                try {
                    logger.debug("Executing job: %s (%s)", jobId, triggerType);
                    callback();
                } catch (const std::exception& e) {
                    ok = false;
                    error = e.what();
                } catch (...) {
                    ok = false;
                    error = "Unknown exception";
                }

                auto self = weak_self.lock();
                if (!self) {
                    logger.warning("Scheduler destroyed before job %s completed; state not recorded", jobId);
                    return;
                }

                std::lock_guard<std::mutex> lock(self->jobsMutex_);
                auto it = self->jobs_.find(jobId);
                if (it == self->jobs_.end())
                    return; // unregistered while running
                Job &job = *it->second;
                if (ok) {
                    job.state.store(JobState::IDLE);
                    job.consecutiveFailures.store(0);
                    job.lastRun = std::chrono::steady_clock::now();
                    logger.debug("Job %s completed successfully", jobId);
                } else {
                    self->handleJobFailure(job, error);
                } });
        }
        catch (...)
        {
            job.state.store(JobState::IDLE);
            throw;
        }
        return true;
    }

    void SchedulerService::handleJobFailure(Job &job, const std::string &error)
    {
        Poco::Logger &logger = Poco::Logger::get("SchedulerService");
        job.state.store(JobState::IDLE); // Always reset to IDLE
        job.consecutiveFailures.fetch_add(1);
        job.totalFailures.fetch_add(1);

        // Log every failure individually
        logger.error("Job %s failed: %s (consecutive failures: %d, total failures: %d)",
                     job.jobId, error, job.consecutiveFailures.load(), job.totalFailures.load());

        // Check if we should shutdown the server
        if (job.consecutiveFailures.load() >= job.maxConsecutiveFailures)
        {
            logger.error("Job %s has failed %d times consecutively, shutting down server",
                         job.jobId, job.consecutiveFailures.load());

            std::function<void()> cb;
            {
                std::lock_guard<std::mutex> lock(callbackMutex_);
                cb = shutdownCallback_;
            }
            if (cb)
            {
                cb();
            }
            else
            {
                logger.error("No shutdown callback set, cannot shutdown server");
            }
        }
    }
}
