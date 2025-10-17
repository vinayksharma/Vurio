#include "orchestration/thread_pool_manager.hpp"
#include <Poco/Environment.h>
#include <Poco/Exception.h>
#include <Poco/Logger.h>
#include <cmath>
#include <thread>
#include <sstream>

namespace MediaDedup
{
    ThreadPoolManager::ThreadPoolManager(std::shared_ptr<UnifiedObservableConfigManager> cfg)
        : cfg_(std::move(cfg)), effective_max_(0), kill_timeout_(std::chrono::seconds(10)), idle_timeout_seconds_(60), pool_(std::make_unique<Poco::ThreadPool>(1, 1, 60))
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
        // Use 75% of hardware threads to avoid oversubscription
        // Poco suggests (cores + 1), we use 75% of that for headroom
        int procs = Poco::Environment::processorCount();
        if (procs <= 0)
            procs = 1;

        // Calculate 75% of Poco's suggested value (cores + 1)
        size_t poco_suggested = static_cast<size_t>(procs + 1);
        size_t cap = static_cast<size_t>(poco_suggested * 0.75);

        // Ensure minimum of 4 threads
        if (cap < 4)
            cap = 4;

        Poco::Logger::get("ThreadPoolManager").debug("Auto thread detection: %d cores detected, Poco suggests %zu, using 75%% = %zu threads", procs, poco_suggested, cap);
        return cap;
    }

    void ThreadPoolManager::initialize()
    {
        Poco::Logger &logger = Poco::Logger::get("ThreadPoolManager");
        logger.information("Initializing ThreadPoolManager...");

        // Read config
        std::string maxKey = "tpm.pool.max";
        std::string killKey = "tpm.killTimeoutMs";
        std::string maxStr = cfg_->getPropertyValue<std::string>(maxKey, std::string("auto"));
        if (maxStr == "auto")
        {
            effective_max_ = defaultPoolMaxFromPoco();
            logger.information("Using auto-detected max threads: %u", static_cast<unsigned int>(effective_max_));
        }
        else
        {
            try
            {
                effective_max_ = static_cast<size_t>(std::stoul(maxStr));
                logger.information("Using configured max threads: %u", static_cast<unsigned int>(effective_max_));
            }
            catch (...)
            {
                effective_max_ = defaultPoolMaxFromPoco();
                logger.warning("Invalid max threads configuration, using auto-detected: %u", static_cast<unsigned int>(effective_max_));
            }
        }

        int killMs = cfg_->getPropertyValue<int>(killKey, 10000);
        if (killMs < 0)
            killMs = 10000;
        kill_timeout_ = std::chrono::milliseconds(killMs);
        logger.debug("Kill timeout set to: %d ms", killMs);

        // Read idle timeout configuration
        std::string idleKey = "tpm.thread.idleTimeoutSeconds";
        int idleSeconds = cfg_->getPropertyValue<int>(idleKey, 120);
        if (idleSeconds <= 0)
            idleSeconds = 120;
        idle_timeout_seconds_ = idleSeconds;
        logger.debug("Thread idle timeout set to: %d seconds", idleSeconds);

        // Read stack size configuration (memory optimization)
        std::string stackSizeKey = "tpm.thread.stackSizeKB";
        int stackSizeKB = cfg_->getPropertyValue<int>(stackSizeKey, 1024); // Default 1MB (1024KB)
        if (stackSizeKB <= 0)
            stackSizeKB = 1024;
        int stackSizeBytes = stackSizeKB * 1024;
        logger.debug("Thread stack size set to: %d KB (%d bytes)", stackSizeKB, stackSizeBytes);

        // Recreate pool with configured idle timeout and desired capacity
        pool_ = std::make_unique<Poco::ThreadPool>(1, static_cast<int>(effective_max_), idle_timeout_seconds_);

        // Set optimized stack size to reduce memory usage (8MB default → 1MB configured)
        pool_->setStackSize(stackSizeBytes);

        logger.information("Thread pool initialized with capacity: %u, idle timeout: %d seconds, stack size: %d KB",
                           static_cast<unsigned int>(effective_max_), idle_timeout_seconds_, stackSizeKB);

        // Subscribe to config changes
        cfg_->subscribeToConfigChanges([this](const ConfigChangeEvent &ev)
                                       { onConfigChange(ev); });
        logger.information("ThreadPoolManager initialized successfully");
    }

    void ThreadPoolManager::shutdownAndDrain(std::chrono::milliseconds killTimeout)
    {
        Poco::Logger &logger = Poco::Logger::get("ThreadPoolManager");
        logger.information("Starting ThreadPoolManager shutdown (timeout: %ld ms)", static_cast<long>(killTimeout.count()));

        accepting_.store(false);
        draining_.store(true);

        // Wait until running_total_ becomes 0 or timeout
        std::unique_lock<std::mutex> lock(mutex_);
        logger.debug("Waiting for %u running tasks to complete...", static_cast<unsigned int>(running_total_));

        bool completed = cv_.wait_for(lock, killTimeout, [this]()
                                      { return running_total_ == 0 && this->active_runnables_.empty(); });

        if (completed)
        {
            logger.information("All tasks completed gracefully");
        }
        else
        {
            logger.warning("Shutdown timeout reached, clearing remaining queues");
        }

        // After timeout, stop all threads; Poco::ThreadPool doesn't preempt tasks, so just clear queues
        type_to_queue_.clear();
        round_robin_types_.clear();
        logger.information("ThreadPoolManager shutdown complete");
    }

    void ThreadPoolManager::setShare(const std::string &type, double share)
    {
        if (share <= 0)
            share = 1.0;
        std::lock_guard<std::mutex> lock(mutex_);
        type_to_share_[type] = share;
    }

    void ThreadPoolManager::submit(const std::string &type, std::function<void()> fn, const std::string &file_path)
    {
        if (!accepting_.load())
        {
            Poco::Logger &logger = Poco::Logger::get("ThreadPoolManager");
            logger.warning("Rejecting task submission for type '%s' - not accepting new tasks", type);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            type_to_queue_[type].emplace_back(std::move(fn), file_path);
            // maintain round robin order
            if (std::find(round_robin_types_.begin(), round_robin_types_.end(), type) == round_robin_types_.end())
            {
                round_robin_types_.push_back(type);
                Poco::Logger &logger = Poco::Logger::get("ThreadPoolManager");
                logger.debug("Added new task type '%s' to round-robin queue", type);
            }
        }

        Poco::Logger &logger = Poco::Logger::get("ThreadPoolManager");
        std::stringstream thread_id;
        thread_id << std::this_thread::get_id();
        logger.debug("Submitted task for type '%s', queue size: %u, file: %s, calling thread: %s",
                     type, static_cast<unsigned int>(type_to_queue_[type].size()),
                     file_path.empty() ? "<no_file>" : file_path.c_str(), thread_id.str());
        schedule();
    }

    bool ThreadPoolManager::canSubmit(const std::string &type, size_t max_queue_size) const
    {
        if (!accepting_.load())
        {
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        auto it = type_to_queue_.find(type);
        size_t current_queue_size = (it != type_to_queue_.end()) ? it->second.size() : 0;

        return current_queue_size < max_queue_size;
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
        // Keep this runnable alive for the duration of run to avoid self-destruction
        auto keepAlive = owner_.lockRunnable(id_);
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
        // No-op: we incremented before start to enforce allowance strictly
        (void)type;
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

    void ThreadPoolManager::schedule()
    {
        if (draining_.load())
        {
            Poco::Logger &logger = Poco::Logger::get("ThreadPoolManager");
            logger.debug("Skipping schedule - draining in progress");
            return;
        }

        std::unique_lock<std::mutex> lock(mutex_);

        // Gradual decrease policy: if running_total_ >= effective_max_, we can still finish tasks but avoid starting new ones
        if (running_total_ >= effective_max_)
        {
            Poco::Logger &logger = Poco::Logger::get("ThreadPoolManager");
            std::stringstream thread_id;
            thread_id << std::this_thread::get_id();
            logger.debug("Skipping schedule - at capacity (%u/%u threads), calling thread: %s",
                         static_cast<unsigned int>(running_total_), static_cast<unsigned int>(effective_max_), thread_id.str());
            return;
        }

        if (round_robin_types_.empty())
        {
            Poco::Logger &logger = Poco::Logger::get("ThreadPoolManager");
            logger.debug("Skipping schedule - no task types registered");
            return;
        }

        Poco::Logger &logger = Poco::Logger::get("ThreadPoolManager");
        logger.debug("Scheduling tasks - running: %u/%u, types: %u", static_cast<unsigned int>(running_total_), static_cast<unsigned int>(effective_max_), static_cast<unsigned int>(round_robin_types_.size()));

        // Make a copy of the round_robin_types_ vector to avoid modification during iteration
        std::vector<std::string> types_copy = round_robin_types_;

        size_t startIndex = rr_index_;
        size_t numTypes = types_copy.size();
        size_t tasksStarted = 0;

        for (size_t i = 0; i < numTypes && running_total_ < effective_max_; ++i)
        {
            const std::string &type_ref = types_copy[(startIndex + i) % numTypes];
            std::string type = type_ref; // Make a copy to avoid reference issues
            auto &queue = type_to_queue_[type];
            if (queue.empty())
                continue;

            size_t runningForType = type_to_running_[type];
            size_t allowance = allowanceFor(type);
            if (runningForType >= allowance)
            {
                logger.debug("Type '%s' at allowance limit (%u/%u)", type, static_cast<unsigned int>(runningForType), static_cast<unsigned int>(allowance));
                continue; // this type is at its slice
            }

            if (running_total_ >= effective_max_)
                break;

            if (pool_->available() <= 0)
            {
                logger.debug("No available threads in pool");
                break;
            }

            auto task = queue.front();
            queue.pop_front();
            auto fn = std::move(task.fn);

            // create runnable and start (we ensured a thread is available)
            uint64_t id = next_id_++;
            auto runnable = std::make_shared<FunctionRunnable>(*this, type, std::move(fn), id);
            // Reserve counts before starting to enforce per-type allowance
            running_total_ += 1;
            type_to_running_[type] += 1;
            active_runnables_[id] = runnable;

            // Safety check for type string
            std::string safe_type = type.empty() ? "<empty_type>" : type;
            if (safe_type.find('\0') != std::string::npos)
            {
                safe_type = "<invalid_type>";
            }

            // Debug: Print type string in hex to see what's actually in it
            std::string hex_debug = "type_hex: ";
            for (char c : type)
            {
                hex_debug += std::to_string(static_cast<unsigned char>(c)) + " ";
            }
            logger.trace("DEBUG: " + hex_debug);

            logger.trace("Starting execution of task " + std::to_string(static_cast<unsigned long long>(id)) + " for type '" + safe_type + "'");
            pool_->start(*runnable);
            tasksStarted++;

            // Log using safe string concatenation to avoid format specifier issues
            logger.debug(std::string("Started task ") +
                         std::to_string(static_cast<unsigned long long>(id)) +
                         " for type '" + safe_type + "' (running: " +
                         std::to_string(static_cast<unsigned int>(type_to_running_[type])) + "/" +
                         std::to_string(static_cast<unsigned int>(running_total_)) + ", allowance: " +
                         std::to_string(static_cast<unsigned int>(allowance)) + ")");

            // record selection index
            rr_index_ = (startIndex + i + 1) % numTypes;
        }

        if (tasksStarted > 0)
        {
            logger.information("Scheduled %u new tasks, total running: %u/%u", static_cast<unsigned int>(tasksStarted), static_cast<unsigned int>(running_total_), static_cast<unsigned int>(effective_max_));
        }
    }

    std::shared_ptr<ThreadPoolManager::FunctionRunnable> ThreadPoolManager::lockRunnable(uint64_t id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = active_runnables_.find(id);
        if (it != active_runnables_.end())
            return it->second;
        return nullptr;
    }

    void ThreadPoolManager::recreateThreadPool()
    {
        Poco::Logger &logger = Poco::Logger::get("ThreadPoolManager");
        logger.information("Recreating thread pool with idle timeout: %d seconds", idle_timeout_seconds_);

        // Wait for current tasks to complete
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this]()
                 { return running_total_ == 0 && active_runnables_.empty(); });

        // Clear queues
        type_to_queue_.clear();
        round_robin_types_.clear();

        // Recreate the thread pool with new idle timeout
        pool_ = std::make_unique<Poco::ThreadPool>(1, static_cast<int>(effective_max_), idle_timeout_seconds_);

        // Apply configured stack size to recreated pool
        std::string stackSizeKey = "tpm.thread.stackSizeKB";
        int stackSizeKB = cfg_->getPropertyValue<int>(stackSizeKey, 1024); // Default 1MB (1024KB)
        if (stackSizeKB <= 0)
            stackSizeKB = 1024;
        int stackSizeBytes = stackSizeKB * 1024;
        pool_->setStackSize(stackSizeBytes);

        logger.information("Thread pool recreated successfully with capacity: %d, idle timeout: %d seconds, stack size: %d KB",
                           static_cast<int>(effective_max_), idle_timeout_seconds_, stackSizeKB);
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
                int cur = pool_->capacity();
                effective_max_ = newMax;

                Poco::Logger::get("ThreadPoolManager").information("Thread pool max changed from %d to %u (current capacity: %d)", cur, static_cast<unsigned int>(newMax), cur);

                if (static_cast<size_t>(cur) < effective_max_)
                {
                    int add_capacity = static_cast<int>(effective_max_ - static_cast<size_t>(cur));
                    Poco::Logger::get("ThreadPoolManager").information("Increasing thread pool capacity by %d threads", add_capacity);
                    pool_->addCapacity(add_capacity);
                }
                else
                {
                    Poco::Logger::get("ThreadPoolManager").information("Keeping current capacity %d (gradual decrease to %u)", cur, static_cast<unsigned int>(effective_max_));
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
        else if (event.key == "tpm.thread.idleTimeoutSeconds")
        {
            try
            {
                int newIdleTimeout = 120;
                if (event.new_value.type() == typeid(int))
                    newIdleTimeout = std::any_cast<int>(event.new_value);
                else if (event.new_value.type() == typeid(std::string))
                    newIdleTimeout = std::stoi(std::any_cast<std::string>(event.new_value));

                if (newIdleTimeout > 0 && newIdleTimeout != idle_timeout_seconds_)
                {
                    idle_timeout_seconds_ = newIdleTimeout;
                    Poco::Logger::get("ThreadPoolManager").information("Thread idle timeout changed to: %d seconds, recreating thread pool", newIdleTimeout);
                    recreateThreadPool();
                }
            }
            catch (...)
            {
                Poco::Logger::get("ThreadPoolManager").warning("Invalid thread idle timeout value, keeping current: %d seconds", idle_timeout_seconds_);
            }
        }
        else if (event.key == "tpm.thread.stackSizeKB")
        {
            try
            {
                int newStackSizeKB = 1024; // Default 1MB
                if (event.new_value.type() == typeid(int))
                    newStackSizeKB = std::any_cast<int>(event.new_value);
                else if (event.new_value.type() == typeid(std::string))
                    newStackSizeKB = std::stoi(std::any_cast<std::string>(event.new_value));

                if (newStackSizeKB > 0)
                {
                    Poco::Logger::get("ThreadPoolManager").information("Thread stack size changed to: %d KB, recreating thread pool", newStackSizeKB);
                    recreateThreadPool();
                }
                else
                {
                    Poco::Logger::get("ThreadPoolManager").warning("Invalid stack size value: %d KB, keeping current configuration", newStackSizeKB);
                }
            }
            catch (...)
            {
                Poco::Logger::get("ThreadPoolManager").warning("Invalid stack size value, keeping current configuration");
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

size_t MediaDedup::ThreadPoolManager::getQueueDepth(const std::string &type) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = type_to_queue_.find(type);
    return (it != type_to_queue_.end()) ? it->second.size() : 0;
}

std::unordered_map<std::string, size_t> MediaDedup::ThreadPoolManager::getAllQueueDepths() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::unordered_map<std::string, size_t> queue_depths;

    for (const auto &pair : type_to_queue_)
    {
        queue_depths[pair.first] = pair.second.size();
    }

    return queue_depths;
}

std::vector<std::string> MediaDedup::ThreadPoolManager::getPendingFilePaths(const std::string &type) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> file_paths;

    auto it = type_to_queue_.find(type);
    if (it != type_to_queue_.end())
    {
        for (const auto &task : it->second)
        {
            if (!task.file_path.empty())
            {
                file_paths.push_back(task.file_path);
            }
        }
    }

    return file_paths;
}

std::unordered_map<std::string, std::vector<std::string>> MediaDedup::ThreadPoolManager::getAllPendingFilePaths() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::unordered_map<std::string, std::vector<std::string>> all_pending;

    for (const auto &pair : type_to_queue_)
    {
        std::vector<std::string> file_paths;
        for (const auto &task : pair.second)
        {
            if (!task.file_path.empty())
            {
                file_paths.push_back(task.file_path);
            }
        }
        if (!file_paths.empty())
        {
            all_pending[pair.first] = std::move(file_paths);
        }
    }

    return all_pending;
}
