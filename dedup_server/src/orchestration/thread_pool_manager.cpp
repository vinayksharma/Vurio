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
        : cfg_(std::move(cfg)), effective_max_(0), idle_timeout_seconds_(60), pool_(std::make_unique<Poco::ThreadPool>(1, 1, 60))
    {
    }

    ThreadPoolManager::~ThreadPoolManager()
    {
        try
        {
            shutdownAndDrain(std::chrono::milliseconds(kill_timeout_ms_.load()));
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
        kill_timeout_ms_.store(killMs);
        logger.debug("Kill timeout set to: %d ms", killMs);

        // Read idle timeout configuration
        std::string idleKey = "tpm.thread.idleTimeoutSeconds";
        int idleSeconds = cfg_->getPropertyValue<int>(idleKey, 120);
        if (idleSeconds <= 0)
            idleSeconds = 120;
        idle_timeout_seconds_ = idleSeconds;
        logger.debug("Thread idle timeout set to: %d seconds", idleSeconds);

        // Combined cap on concurrently running thumbnail/preview generation
        // tasks (each runs an external magick decode of up to 100-200MB).
        int mediaCap = cfg_->getPropertyValue<int>("tpm.mediaGeneration.maxConcurrent", 4);
        if (mediaCap < 1)
            mediaCap = 1;
        media_generation_cap_ = static_cast<size_t>(mediaCap);
        logger.debug("Media generation concurrency cap set to: %d", mediaCap);

        // Read stack size configuration (memory optimization)
        std::string stackSizeKey = "tpm.thread.stackSizeKB";
        int stackSizeKB = cfg_->getPropertyValue<int>(stackSizeKey, 1024); // Default 1MB (1024KB)
        if (stackSizeKB <= 0)
            stackSizeKB = 1024;
        int stackSizeBytes = stackSizeKB * 1024;
        logger.debug("Thread stack size set to: %d KB (%d bytes)", stackSizeKB, stackSizeBytes);

        // Recreate pool with configured idle timeout and desired capacity
        // We set the underlying Poco::ThreadPool capacity to effective_max_ * 2.
        // ThreadPoolManager manually strictly limits running tasks to effective_max_.
        // The extra slack allows new tasks to start immediately even if finishing threads
        // are still executing onTaskComplete() and haven't technically become idle yet,
        // avoiding NoThreadAvailableException and scheduling stalls.
        int poco_pool_capacity = static_cast<int>(effective_max_ * 2);
        pool_ = std::make_unique<Poco::ThreadPool>(1, poco_pool_capacity, idle_timeout_seconds_);

        // Set optimized stack size to reduce memory usage (8MB default → 1MB configured)
        pool_->setStackSize(stackSizeBytes);

        logger.information("Thread pool initialized with capacity: %u, idle timeout: %d seconds, stack size: %d KB",
                           static_cast<unsigned int>(effective_max_), idle_timeout_seconds_, stackSizeKB);

        // Subscribe to config changes
        config_subscription_ = ScopedConfigSubscription(
            cfg_, cfg_->subscribeToConfigChanges([this](const ConfigChangeEvent &ev)
                                                 { onConfigChange(ev); }));
                                       
        // Load any existing share configurations
        for (const auto& key : cfg_->getAllPropertyKeys())
        {
            if (key.rfind("tpm.types.", 0) == 0 && key.rfind(".share") != std::string::npos)
            {
                std::string type = key.substr(std::string("tpm.types.").size());
                size_t pos = type.rfind(".share");
                if (pos != std::string::npos)
                {
                    type = type.substr(0, pos);
                    double share = 1.0;
                    try
                    {
                        // Safely try to fetch as double
                        share = cfg_->getPropertyValue<double>(key, 1.0);
                    }
                    catch (...)
                    {
                    }
                    setShare(type, share);
                }
            }
        }
                                       
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

    void ThreadPoolManager::clearQueues()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        type_to_queue_.clear();
        round_robin_types_.clear();
        // CC-6: rebuild the dedup index from the tasks still RUNNING.
        inflight_paths_.clear();
        for (const auto &pair : active_runnables_)
        {
            if (!pair.second->file_path().empty())
                inflight_paths_.insert(pair.second->type() + "|" + pair.second->file_path());
        }
        Poco::Logger::get("ThreadPoolManager").information("Queues cleared successfully");
    }

    void ThreadPoolManager::setShare(const std::string &type, double share)
    {
        if (share < 0)
            share = 1.0;
        std::lock_guard<std::mutex> lock(mutex_);
        type_to_share_[type] = share;
    }

    void ThreadPoolManager::setTemporaryOverride(const std::string &type, double temporary_share)
    {
        if (temporary_share < 0)
            temporary_share = 1.0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            type_to_temp_override_[type] = temporary_share;
        }
        Poco::Logger::get("ThreadPoolManager").information("Temporary share override applied for type '%s': %f", type, temporary_share);
        schedule();
    }

    void ThreadPoolManager::clearTemporaryOverride(const std::string &type)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            type_to_temp_override_.erase(type);
        }
        Poco::Logger::get("ThreadPoolManager").information("Temporary share override cleared for type '%s'", type);
        schedule();
    }

    void ThreadPoolManager::submit(const std::string &type, std::function<void()> fn, const std::string &file_path,
                                   bool urgent)
    {
        if (!accepting_.load())
        {
            Poco::Logger &logger = Poco::Logger::get("ThreadPoolManager");
            logger.warning("Rejecting task submission for type '%s' - not accepting new tasks", type);
            return;
        }

        // QW-5: captured under the lock; the logging below must not touch
        // type_to_queue_ unlocked (operator[] mutates and can rehash while
        // schedule()/onTaskFinished mutate the map on worker threads).
        size_t queued_for_type = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);

            // Deduplication check — CC-6: O(1) set membership instead of a
            // linear scan of the queue AND every active runnable under mutex_
            // (O(n^2) across a bulk submission of n files).
            if (!file_path.empty())
            {
                if (!inflight_paths_.insert(type + "|" + file_path).second)
                {
                    // Already queued or running. An urgent submit still pulls
                    // a queued copy to the front so the UI-visible request is
                    // served next instead of at backfill's FIFO position.
                    if (urgent)
                        promoteQueuedTaskLocked(type, file_path);
                    return;
                }
            }

            auto &queue = type_to_queue_[type];
            if (urgent)
                queue.emplace_front(std::move(fn), file_path, true);
            else
                queue.emplace_back(std::move(fn), file_path, false);
            queued_for_type = queue.size();
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
        std::string thread_id_str = thread_id.str();
        logger.debug("Submitted task for type '%s', queue size: %u, file: %s, calling thread: %s",
                     type.c_str(), static_cast<unsigned int>(queued_for_type),
                     file_path.empty() ? "<no_file>" : file_path.c_str(), thread_id_str.c_str());
        schedule();
    }

    void ThreadPoolManager::promoteQueuedTaskLocked(const std::string &type, const std::string &file_path)
    {
        auto it = type_to_queue_.find(type);
        if (it == type_to_queue_.end())
            return;
        auto &queue = it->second;
        for (auto qit = queue.begin(); qit != queue.end(); ++qit)
        {
            if (qit->file_path == file_path)
            {
                // The queued copy was almost certainly enqueued by a bulk
                // producer, so it must inherit the urgency of the request that
                // just deduped into it — otherwise it keeps competing with
                // backfill for the media slots it was promoted out of.
                qit->urgent = true;
                if (qit != queue.begin())
                {
                    QueuedTask task = std::move(*qit);
                    queue.erase(qit);
                    queue.push_front(std::move(task));
                }
                return;
            }
        }
        // Not in the queue: the task is already running — nothing to promote.
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

    bool ThreadPoolManager::cancelTask(const std::string &type, const std::string &file_path)
    {
        if (file_path.empty()) return false;

        std::lock_guard<std::mutex> lock(mutex_);
        auto it = type_to_queue_.find(type);
        if (it != type_to_queue_.end())
        {
            auto& queue = it->second;
            auto remove_it = std::remove_if(queue.begin(), queue.end(),
                [&file_path](const QueuedTask& task) { return task.file_path == file_path; });
            
            if (remove_it != queue.end())
            {
                size_t removed_count = std::distance(remove_it, queue.end());
                queue.erase(remove_it, queue.end());
                inflight_paths_.erase(type + "|" + file_path);
                Poco::Logger::get("ThreadPoolManager").debug("Cancelled %lu task(s) of type '%s' for file '%s'", 
                                                            (unsigned long)removed_count, type.c_str(), file_path.c_str());
                return true;
            }
        }
        return false;
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

    struct TaskCleanupHelper
    {
        ThreadPoolManager *owner = nullptr;
        std::string type;
        uint64_t id = 0;
        bool active = false;

        ~TaskCleanupHelper()
        {
            if (active && owner)
            {
                Poco::Logger::get("ThreadPoolManager").warning("TaskCleanupHelper: thread exited abruptly, performing task cleanup for type '" + type + "', id " + std::to_string(id));
                owner->onTaskFinished(type, id);
            }
        }
    };

    void ThreadPoolManager::FunctionRunnable::run()
    {
        // Keep this runnable alive for the duration of run to avoid self-destruction
        auto keepAlive = owner_.lockRunnable(id_);
        
        // CC-6: stack-scoped — thread_local deferred the abrupt-exit guard
        // to THREAD exit and carried stale owner/type/id across tasks.
        TaskCleanupHelper cleanup;
        cleanup.owner = &owner_;
        cleanup.type = type_;
        cleanup.id = id_;
        cleanup.active = true;

        try
        {
            fn_();
        }
        catch (...)
        {
        }
        
        cleanup.active = false;
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
            auto rit = active_runnables_.find(id);
            if (rit != active_runnables_.end())
            {
                if (!rit->second->file_path().empty())
                    inflight_paths_.erase(type + "|" + rit->second->file_path());
                active_runnables_.erase(rit);
            }
        }
        cv_.notify_all();
        schedule();
    }

    size_t ThreadPoolManager::allowanceFor(const std::string &type) const
    {
        double share = 1.0;
        
        auto temp_it = type_to_temp_override_.find(type);
        if (temp_it != type_to_temp_override_.end())
        {
            share = temp_it->second;
        }
        else
        {
            auto it = type_to_share_.find(type);
            share = (it == type_to_share_.end()) ? 1.0 : it->second;
        }
        
        if (share == 0.0)
            return 0; // Explicitly allow 0 for pausing

        size_t allow = static_cast<size_t>(std::ceil(share * static_cast<double>(effective_max_)));
        if (allow == 0)
            allow = 1; // ensure at least one if share > 0
        return allow;
    }

    void ThreadPoolManager::resume()
    {
        paused_.store(false);
        schedule();
    }

    void ThreadPoolManager::schedule()
    {
        if (draining_.load() || paused_.load())
        {
            Poco::Logger &logger = Poco::Logger::get("ThreadPoolManager");
            logger.debug("Skipping schedule - draining or paused");
            return;
        }

        std::unique_lock<std::mutex> lock(mutex_);

        // Media task types, most user-visible first. Urgent work is served in
        // this order rather than by round-robin position: the image filling
        // the viewer outranks the strip tile behind it.
        static constexpr const char *kMediaTypes[] = {"fullres_generator", "preview_generator", "thumbnail_generator"};

        // An urgent task always sits at the front of its type's queue (urgent
        // submits push_front, everything else push_back), so the head answers
        // "is a user waiting on this type?" in O(1).
        auto type_head_is_urgent = [this](const char *t) {
            auto it = type_to_queue_.find(t);
            return it != type_to_queue_.end() && !it->second.empty() && it->second.front().urgent;
        };

        // Check for high-priority UI tasks dynamically. This must mean "a user
        // is actually waiting", not merely "a media queue is non-empty" — the
        // backfill enqueues up to 20,000 non-urgent thumbnail/preview jobs into
        // these same queues, and treating those as UI work kept every
        // background type throttled to one thread for the entire sweep.
        auto has_ui_tasks_fn = [&type_head_is_urgent]() {
            for (const char *t : kMediaTypes)
                if (type_head_is_urgent(t)) return true;
            return false;
        };

        size_t current_limit = effective_max_;
        if (has_ui_tasks_fn())
        {
            current_limit = static_cast<size_t>(pool_->capacity());
        }

        // Gradual decrease policy: if running_total_ >= current_limit, we can still finish tasks but avoid starting new ones
        if (running_total_ >= current_limit)
        {
            Poco::Logger &logger = Poco::Logger::get("ThreadPoolManager");
            std::stringstream thread_id;
            thread_id << std::this_thread::get_id();
            logger.debug("Skipping schedule - at capacity (%u/%u threads), calling thread: %s",
                         static_cast<unsigned int>(running_total_), static_cast<unsigned int>(current_limit), thread_id.str());
            return;
        }

        if (round_robin_types_.empty())
        {
            Poco::Logger &logger = Poco::Logger::get("ThreadPoolManager");
            logger.debug("Skipping schedule - no task types registered");
            return;
        }
        Poco::Logger &logger = Poco::Logger::get("ThreadPoolManager");
        logger.debug("Scheduling tasks - running: %u/%u, types: %u", static_cast<unsigned int>(running_total_), static_cast<unsigned int>(current_limit), static_cast<unsigned int>(round_robin_types_.size()));

        // Make a copy of the round_robin_types_ vector to avoid modification during iteration
        std::vector<std::string> types_copy = round_robin_types_;
        size_t startIndex = rr_index_;
        size_t numTypes = types_copy.size();
        size_t tasksStarted = 0;

        // Hoist media types whose head is urgent ahead of the round-robin, in
        // kMediaTypes order. Front-of-queue placement only ordered a task
        // against its own type; across types the rotation could still hand the
        // next free media slot to a backfill thumbnail while the viewer's
        // render waited its turn.
        {
            std::vector<std::string> ordered;
            ordered.reserve(numTypes);
            for (const char *t : kMediaTypes)
            {
                if (!type_head_is_urgent(t))
                    continue;
                if (std::find(types_copy.begin(), types_copy.end(), t) != types_copy.end())
                    ordered.emplace_back(t);
            }
            if (!ordered.empty())
            {
                for (size_t i = 0; i < numTypes; ++i)
                {
                    const std::string &t = types_copy[(startIndex + i) % numTypes];
                    if (std::find(ordered.begin(), ordered.end(), t) == ordered.end())
                        ordered.push_back(t);
                }
                types_copy = std::move(ordered);
                startIndex = 0;
                numTypes = types_copy.size();
            }
        }

        bool task_started_in_pass;
        do {
            task_started_in_pass = false;
            
            for (size_t i = 0; i < numTypes && running_total_ < current_limit; ++i)
            {
                const std::string &type_ref = types_copy[(startIndex + i) % numTypes];
                std::string type = type_ref; // Make a copy to avoid reference issues
                
                // Auto-prioritization: while UI tasks are waiting, background
                // types are THROTTLED to one running task, not skipped. CC-3:
                // the old unconditional skip starved fileScan/mediaProcessor/
                // duplicateFinder for as long as the user kept scrolling
                // (thumbnails queue continuously) — background work froze
                // indefinitely under a busy UI.
                const bool is_ui_type = (type == "thumbnail_generator" || type == "preview_generator" || type == "fullres_generator");
                const bool ui_backlog = !is_ui_type && has_ui_tasks_fn();

            auto &queue = type_to_queue_[type];
            if (queue.empty())
                continue;

            size_t runningForType = type_to_running_[type];
            size_t allowance = allowanceFor(type);
            if (ui_backlog)
                allowance = std::min<size_t>(allowance, 1);

            // Media generation (thumbnail/preview) spawns an external magick
            // decoder per task. It stays exempt from the per-type share slices
            // (so the UI is responsive during heavy processing) but is bounded
            // by a combined cap — the old "use every thread" exemption let RAF
            // bursts run dozens of parallel demosaics, which is what caused
            // mass transient transcode failures under memory pressure.
            if (is_ui_type)
            {
                size_t media_running = type_to_running_["thumbnail_generator"] + type_to_running_["preview_generator"] + type_to_running_["fullres_generator"];

                // The last slot belongs to user-visible work. Without it, a
                // backfill sweep keeps all `media_generation_cap_` slots busy
                // with multi-second magick decodes and the viewer's urgent
                // request — front of its queue, first in the type order — still
                // waits for one of them to finish, because nothing preempts a
                // running task. Bulk work therefore runs at cap-1 and an
                // urgent arrival always finds a slot free.
                size_t cap = media_generation_cap_;
                if (!queue.front().urgent && cap > 1)
                    cap -= 1;
                if (media_running >= cap)
                {
                    continue;
                }
                allowance = current_limit;
            }
            if (runningForType >= allowance)
            {
                logger.debug("Type '%s' at allowance limit (%u/%u)", type, static_cast<unsigned int>(runningForType), static_cast<unsigned int>(allowance));
                continue; // this type is at its slice
            }

            if (running_total_ >= current_limit)
                break;

            // Only genuinely user-visible work may bypass effective_max_. The
            // type name alone used to grant this, which handed backfill's
            // thumbnail/preview jobs the right to oversubscribe the pool.
            if (running_total_ >= effective_max_ && !(is_ui_type && queue.front().urgent))
            {
                continue;
            }

            // Move (not copy) the task out of the queue; on a failed start the
            // callable is reclaimed from the runnable via takeFn() (CC-2).
            auto task = std::move(queue.front());
            queue.pop_front();
            auto fn = std::move(task.fn);
            std::string task_file_path = std::move(task.file_path);
            const bool task_urgent = task.urgent; // preserved across the CC-2 re-queues below

            // create runnable and start (we ensured a thread is available)
            uint64_t id = next_id_++;
            auto runnable = std::make_shared<FunctionRunnable>(*this, type, std::move(fn), id, std::move(task_file_path));
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
            try
            {
                startOnPool(*runnable);
                tasksStarted++;
                task_started_in_pass = true;

                // Log using safe string concatenation to avoid format specifier issues
                logger.debug(std::string("Started task ") +
                             std::to_string(static_cast<unsigned long long>(id)) +
                             " for type '" + safe_type + "' (running: " +
                             std::to_string(static_cast<unsigned int>(type_to_running_[type])) + "/" +
                             std::to_string(static_cast<unsigned int>(running_total_)) + ", allowance: " +
                             std::to_string(static_cast<unsigned int>(allowance)) + ")");
            }
            catch (const Poco::NoThreadAvailableException& e)
            {
                logger.warning("No threads available in pool. Pushing task " + std::to_string(static_cast<unsigned long long>(id)) + " back to queue.");
                if (running_total_ > 0)
                    running_total_ -= 1;
                auto it = type_to_running_.find(type);
                if (it != type_to_running_.end() && it->second > 0)
                    it->second -= 1;
                active_runnables_.erase(id);

                // CC-2: reclaim the callable from the never-started runnable
                // and re-queue an INTACT task. The old code pushed back the
                // moved-from `task` whose fn was already empty — it later ran
                // as bad_function_call (swallowed), the work was silently
                // lost, and its file_path blocked resubmission via the dedup
                // check until the zombie drained.
                queue.emplace_front(runnable->takeFn(), runnable->file_path(), task_urgent);
                break; // Stop scheduling
            }
            catch (const std::exception& e)
            {
                logger.error("Failed to start task " + std::to_string(static_cast<unsigned long long>(id)) + " for type '" + safe_type + "': " + std::string(e.what()) + " — re-queued for the next scheduling pass");
                if (running_total_ > 0)
                    running_total_ -= 1;
                auto it = type_to_running_.find(type);
                if (it != type_to_running_.end() && it->second > 0)
                    it->second -= 1;
                active_runnables_.erase(id);
                // CC-2: these paths previously DROPPED the task with no
                // re-queue and no signal to the submitter. Re-queue intact and
                // stop this pass; the next schedule() event retries.
                queue.emplace_front(runnable->takeFn(), runnable->file_path(), task_urgent);
                break;
            }
            catch (...)
            {
                logger.error("Failed to start task " + std::to_string(static_cast<unsigned long long>(id)) + " for type '" + safe_type + "' due to unknown exception — re-queued for the next scheduling pass");
                if (running_total_ > 0)
                    running_total_ -= 1;
                auto it = type_to_running_.find(type);
                if (it != type_to_running_.end() && it->second > 0)
                    it->second -= 1;
                active_runnables_.erase(id);
                queue.emplace_front(runnable->takeFn(), runnable->file_path(), task_urgent);
                break;
            }

            // record selection index
            rr_index_ = (startIndex + i + 1) % numTypes;
        }
    } while (task_started_in_pass && running_total_ < current_limit);

        if (tasksStarted > 0)
        {
            logger.information("Scheduled %u new tasks, total running: %u/%u (limit: %u)",
                               static_cast<unsigned int>(tasksStarted),
                               static_cast<unsigned int>(running_total_),
                               static_cast<unsigned int>(effective_max_),
                               static_cast<unsigned int>(current_limit));
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

    // CC-1: recreateThreadPool() is deleted. It waited on cv_ (no timeout)
    // while HOLDING mutex_, then destroyed the Poco pool — whose join waits
    // for workers that were themselves blocked on mutex_ inside schedule():
    // a hard deadlock of every TPM entry point, reachable from any config
    // edit touching the thread settings. It also silently discarded every
    // queued task. Idle timeout now requires a restart (Poco pools cannot
    // change it live — the DatabaseManager pool-size precedent); stack size
    // applies live via setStackSize (affects only future threads).

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
        else if (event.key == "tpm.mediaGeneration.maxConcurrent")
        {
            try
            {
                int cap = 4;
                if (event.new_value.type() == typeid(int))
                    cap = std::any_cast<int>(event.new_value);
                else if (event.new_value.type() == typeid(std::string))
                    cap = std::stoi(std::any_cast<std::string>(event.new_value));
                if (cap < 1)
                    cap = 1;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    media_generation_cap_ = static_cast<size_t>(cap);
                }
                Poco::Logger::get("ThreadPoolManager").information("Media generation concurrency cap changed to %d", cap);
                schedule();
            }
            catch (...)
            {
            }
        }
        else if (event.key == "tpm.killTimeoutMs")
        {
            try
            {
                if (event.new_value.type() == typeid(int))
                    kill_timeout_ms_.store(std::any_cast<int>(event.new_value));
                else if (event.new_value.type() == typeid(std::string))
                    kill_timeout_ms_.store(std::stoi(std::any_cast<std::string>(event.new_value)));
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
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        idle_timeout_seconds_ = newIdleTimeout;
                    }
                    Poco::Logger::get("ThreadPoolManager").information(
                        "Thread idle timeout changed to: %d seconds — takes effect after server restart (live pool recreation was deadlock-prone, CC-1)",
                        newIdleTimeout);
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
                    // Applies to threads created from now on; running threads
                    // keep their stacks. No recreation needed (CC-1).
                    std::lock_guard<std::mutex> lock(mutex_);
                    pool_->setStackSize(newStackSizeKB * 1024);
                    Poco::Logger::get("ThreadPoolManager").information("Thread stack size changed to: %d KB (applies to newly created pool threads)", newStackSizeKB);
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
