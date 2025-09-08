#include "orchestration/thread_pool_manager.hpp"
#include <Poco/Environment.h>
#include <cmath>

namespace MediaDedup
{
    ThreadPoolManager::ThreadPoolManager(std::shared_ptr<UnifiedObservableConfigManager> cfg)
        : cfg_(std::move(cfg)), effective_max_(0), kill_timeout_(std::chrono::seconds(10)), pool_(1, 1)
    {
    }

    ThreadPoolManager::~ThreadPoolManager()
    {
        try
        {
            shutdownAndDrain(kill_timeout_);
        }
        catch (...)
        {
        }
    }

    static size_t defaultPoolMaxFromPoco()
    {
        // Follow user's preference: Poco's default
        // We'll mimic Poco::ThreadPool default capacity selection using CPU count
        int procs = Poco::Environment::processorCount();
        if (procs <= 0)
            procs = 1;
        size_t cap = static_cast<size_t>(procs + 1); // similar to Poco default (n+1)
        if (cap < 4)
            cap = 4;
        return cap;
    }

    void ThreadPoolManager::initialize()
    {
        // Read config
        std::string maxKey = "tpm.pool.max";
        std::string killKey = "tpm.killTimeoutMs";
        std::string maxStr = cfg_->getPropertyValue<std::string>(maxKey, std::string("auto"));
        if (maxStr == "auto")
        {
            effective_max_ = defaultPoolMaxFromPoco();
        }
        else
        {
            try
            {
                effective_max_ = static_cast<size_t>(std::stoul(maxStr));
            }
            catch (...)
            {
                effective_max_ = defaultPoolMaxFromPoco();
            }
        }

        int killMs = cfg_->getPropertyValue<int>(killKey, 10000);
        if (killMs < 0)
            killMs = 10000;
        kill_timeout_ = std::chrono::milliseconds(killMs);

        // Initialize pool capacity to desired effective max
        int current = pool_.capacity();
        if (static_cast<size_t>(current) < effective_max_)
        {
            pool_.addCapacity(static_cast<int>(effective_max_ - static_cast<size_t>(current)));
        }

        // Subscribe to config changes
        cfg_->subscribeToConfigChanges([this](const ConfigChangeEvent &ev)
                                       { onConfigChange(ev); });
    }

    void ThreadPoolManager::shutdownAndDrain(std::chrono::milliseconds killTimeout)
    {
        accepting_.store(false);
        draining_.store(true);
        // Wait until running_total_ becomes 0 or timeout
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, killTimeout, [this]()
                     { return running_total_ == 0 && this->active_runnables_.empty(); });
        // After timeout, stop all threads; Poco::ThreadPool doesn't preempt tasks, so just clear queues
        type_to_queue_.clear();
        round_robin_types_.clear();
    }

    void ThreadPoolManager::setShare(const std::string &type, double share)
    {
        if (share <= 0)
            share = 1.0;
        std::lock_guard<std::mutex> lock(mutex_);
        type_to_share_[type] = share;
    }

    void ThreadPoolManager::submit(const std::string &type, std::function<void()> fn,
                                   std::optional<double> shareOverride)
    {
        if (!accepting_.load())
            return;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            type_to_queue_[type].emplace_back(std::move(fn), shareOverride);
            // maintain round robin order
            if (std::find(round_robin_types_.begin(), round_robin_types_.end(), type) == round_robin_types_.end())
            {
                round_robin_types_.push_back(type);
            }
        }
        schedule();
    }

    ThreadPoolManager::Status ThreadPoolManager::getStatus() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Status s;
        s.effectiveMax = effective_max_;
        s.runningTotal = running_total_;
        for (const auto &t : round_robin_types_)
        {
            TypeStatus ts;
            auto itS = type_to_share_.find(t);
            ts.share = (itS == type_to_share_.end()) ? 1.0 : itS->second;
            auto itR = type_to_running_.find(t);
            ts.running = (itR == type_to_running_.end()) ? 0 : itR->second;
            auto itQ = type_to_queue_.find(t);
            ts.queued = (itQ == type_to_queue_.end()) ? 0 : itQ->second.size();
            s.perType.emplace(t, ts);
        }
        return s;
    }

    void ThreadPoolManager::FunctionRunnable::run()
    {
        owner_.onTaskStarted(type_, id_);
        try
        {
            fn_();
        }
        catch (...)
        {
        }
        owner_.onTaskFinished(type_, id_);
    }

    void ThreadPoolManager::onTaskStarted(const std::string &type, uint64_t id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_total_ += 1;
        type_to_running_[type] += 1;
        (void)id;
    }

    void ThreadPoolManager::onTaskFinished(const std::string &type, uint64_t id)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (running_total_ > 0)
                running_total_ -= 1;
            auto it = type_to_running_.find(type);
            if (it != type_to_running_.end() && it->second > 0)
                it->second -= 1;
            active_runnables_.erase(id);
        }
        cv_.notify_all();
        schedule();
    }

    size_t ThreadPoolManager::allowanceFor(const std::string &type) const
    {
        auto it = type_to_share_.find(type);
        double share = (it == type_to_share_.end()) ? 1.0 : it->second;
        size_t allow = static_cast<size_t>(std::ceil(share * static_cast<double>(effective_max_)));
        if (allow == 0)
            allow = 1; // ensure at least one
        return allow;
    }

    double ThreadPoolManager::resolveShareFor(const std::string &type, std::optional<double> overrideShare) const
    {
        if (overrideShare.has_value())
            return *overrideShare;
        auto it = type_to_share_.find(type);
        if (it != type_to_share_.end())
            return it->second;
        return 1.0;
    }

    void ThreadPoolManager::schedule()
    {
        if (draining_.load())
            return;

        std::unique_lock<std::mutex> lock(mutex_);

        // Gradual decrease policy: if running_total_ >= effective_max_, we can still finish tasks but avoid starting new ones
        if (running_total_ >= effective_max_)
            return;

        if (round_robin_types_.empty())
            return;

        size_t startIndex = rr_index_;
        size_t numTypes = round_robin_types_.size();
        for (size_t i = 0; i < numTypes && running_total_ < effective_max_; ++i)
        {
            const std::string &type = round_robin_types_[(startIndex + i) % numTypes];
            auto &queue = type_to_queue_[type];
            if (queue.empty())
                continue;

            size_t runningForType = type_to_running_[type];
            size_t allowance = allowanceFor(type);
            if (runningForType >= allowance)
                continue; // this type is at its slice

            if (running_total_ >= effective_max_)
                break;

            auto [fn, shareOverride] = queue.front();
            queue.pop_front();

            // create runnable and start
            uint64_t id = next_id_++;
            auto runnable = std::make_shared<FunctionRunnable>(*this, type, std::move(fn), id);
            active_runnables_[id] = runnable;
            // Start using Poco ThreadPool; concurrency limited by pool capacity and our counters
            pool_.start(*runnable);

            // record selection index
            rr_index_ = (startIndex + i + 1) % numTypes;
        }
    }

    void ThreadPoolManager::onConfigChange(const ConfigChangeEvent &event)
    {
        if (event.key == "tpm.pool.max")
        {
            // Handle string or int values
            size_t newMax = effective_max_;
            try
            {
                if (event.new_value.type() == typeid(std::string))
                {
                    std::string v = std::any_cast<std::string>(event.new_value);
                    if (v == "auto")
                        newMax = defaultPoolMaxFromPoco();
                    else
                        newMax = static_cast<size_t>(std::stoul(v));
                }
                else if (event.new_value.type() == typeid(int))
                {
                    int i = std::any_cast<int>(event.new_value);
                    newMax = i > 0 ? static_cast<size_t>(i) : defaultPoolMaxFromPoco();
                }
            }
            catch (...)
            {
                newMax = defaultPoolMaxFromPoco();
            }

            // Gradual decrease: increase capacity immediately; if decreased, just update effective_max_ and avoid starting new ones until under target
            {
                std::lock_guard<std::mutex> lock(mutex_);
                int cur = pool_.capacity();
                effective_max_ = newMax;
                if (static_cast<size_t>(cur) < effective_max_)
                {
                    pool_.addCapacity(static_cast<int>(effective_max_ - static_cast<size_t>(cur)));
                }
                // If current capacity is higher than effective_max_, we keep the pool capacity as-is
                // and rely on the scheduler to enforce the new lower cap (gradual decrease).
            }
            schedule();
        }
        else if (event.key == "tpm.killTimeoutMs")
        {
            try
            {
                if (event.new_value.type() == typeid(int))
                    kill_timeout_ = std::chrono::milliseconds(std::any_cast<int>(event.new_value));
                else if (event.new_value.type() == typeid(std::string))
                    kill_timeout_ = std::chrono::milliseconds(std::stoi(std::any_cast<std::string>(event.new_value)));
            }
            catch (...)
            {
            }
        }
        else if (event.key.rfind("tpm.types.", 0) == 0 && event.key.rfind(".share") != std::string::npos)
        {
            // tpm.types.<name>.share
            std::string type = event.key.substr(std::string("tpm.types.").size());
            size_t pos = type.rfind(".share");
            if (pos != std::string::npos)
                type = type.substr(0, pos);
            double share = 1.0;
            try
            {
                if (event.new_value.type() == typeid(double))
                    share = std::any_cast<double>(event.new_value);
                else if (event.new_value.type() == typeid(int))
                    share = static_cast<double>(std::any_cast<int>(event.new_value));
                else if (event.new_value.type() == typeid(std::string))
                    share = std::stod(std::any_cast<std::string>(event.new_value));
            }
            catch (...)
            {
                share = 1.0;
            }
            setShare(type, share);
            schedule();
        }
    }
}
