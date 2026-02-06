#pragma once

#include <memory>
#include <string>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <unordered_map>

#if defined(HAVE_ONNXRUNTIME)
#include <onnxruntime/core/session/onnxruntime_cxx_api.h>
#endif

namespace MediaDedup
{
    class UnifiedObservableConfigManager;

#if defined(HAVE_ONNXRUNTIME)
    /**
     * @brief Wrapper for ONNX Runtime session with environment
     * Encapsulates both Ort::Env and Ort::Session for complete session lifecycle
     */
    struct OnnxSessionWrapper
    {
        std::unique_ptr<Ort::Env> env;
        std::unique_ptr<Ort::Session> session;
        std::string model_path;

        OnnxSessionWrapper(const std::string &model_path_param);
        ~OnnxSessionWrapper() = default;

        // Non-copyable, movable
        OnnxSessionWrapper(const OnnxSessionWrapper &) = delete;
        OnnxSessionWrapper &operator=(const OnnxSessionWrapper &) = delete;
        OnnxSessionWrapper(OnnxSessionWrapper &&) = default;
        OnnxSessionWrapper &operator=(OnnxSessionWrapper &&) = default;
    };

    /**
     * @brief RAII wrapper for borrowing ONNX session from pool
     * Automatically returns session to pool when destroyed
     */
    class OnnxSessionLease
    {
    public:
        OnnxSessionLease(OnnxSessionWrapper *session,
                         std::function<void(OnnxSessionWrapper *)> return_fn)
            : session_(session), return_fn_(std::move(return_fn)) {}

        ~OnnxSessionLease()
        {
            if (session_ && return_fn_)
            {
                return_fn_(session_);
            }
        }

        // Non-copyable, movable
        OnnxSessionLease(const OnnxSessionLease &) = delete;
        OnnxSessionLease &operator=(const OnnxSessionLease &) = delete;
        OnnxSessionLease(OnnxSessionLease &&other) noexcept
            : session_(other.session_), return_fn_(std::move(other.return_fn_))
        {
            other.session_ = nullptr;
            other.return_fn_ = nullptr;
        }
        OnnxSessionLease &operator=(OnnxSessionLease &&) = delete;

        Ort::Session *getSession() { return session_ ? session_->session.get() : nullptr; }
        Ort::Env *getEnv() { return session_ ? session_->env.get() : nullptr; }
        const std::string &getModelPath() const { return session_->model_path; }

    private:
        OnnxSessionWrapper *session_;
        std::function<void(OnnxSessionWrapper *)> return_fn_;
    };

    /**
     * @brief Thread-safe ONNX session pool manager
     *
     * Manages a pool of reusable ONNX Runtime sessions to avoid expensive
     * model loading on every inference. Sessions are keyed by model path
     * and reused across multiple inference calls.
     *
     * Thread Safety: All public methods are thread-safe
     *
     * Configuration:
     * - media.image.quality.onnx.sessionCache.enabled: Enable/disable caching
     * - media.image.quality.onnx.sessionCache.maxSessions: Max sessions per model
     */
    class OnnxSessionManager
    {
    public:
        explicit OnnxSessionManager(std::shared_ptr<UnifiedObservableConfigManager> config);
        ~OnnxSessionManager();

        /**
         * @brief Borrow a session for the given model path
         *
         * If a session is available in the pool, returns it immediately.
         * If pool is at capacity, may create a new session.
         * If cache is disabled, creates a new session each time.
         *
         * @param model_path Path to the ONNX model file
         * @return OnnxSessionLease RAII wrapper that auto-returns session
         * @throws std::runtime_error if session creation fails
         */
        OnnxSessionLease borrowSession(const std::string &model_path);

        /**
         * @brief Clear all cached sessions
         * Useful for forcing reload after model updates
         */
        void clearCache();

        /**
         * @brief Get pool statistics for monitoring
         */
        struct PoolStats
        {
            size_t total_sessions = 0;
            size_t available_sessions = 0;
            size_t borrowed_sessions = 0;
            bool cache_enabled = false;
            size_t max_sessions_per_model = 0;
        };
        PoolStats getStats() const;

    private:
        std::shared_ptr<UnifiedObservableConfigManager> config_;

        // Session pools per model path
        mutable std::mutex mutex_;
        std::unordered_map<std::string, std::deque<std::unique_ptr<OnnxSessionWrapper>>> session_pools_;

        // Configuration cache (observable)
        bool cache_enabled_ = true;
        size_t max_sessions_per_model_ = 4;

        // Create a new session wrapper
        std::unique_ptr<OnnxSessionWrapper> createSession(const std::string &model_path);

        // Return session to pool (called by OnnxSessionLease destructor)
        void returnSession(OnnxSessionWrapper *session);

        // Configuration change handler
        void onConfigChange();
    };
#else
    // Stub implementations when ONNX Runtime is not available
    struct OnnxSessionWrapper
    {
    };

    class OnnxSessionLease
    {
    public:
        OnnxSessionLease(void *, std::function<void(void *)>) {}
        void *getSession() { return nullptr; }
        void *getEnv() { return nullptr; }
        const std::string &getModelPath() const
        {
            static std::string empty;
            return empty;
        }
    };

    class OnnxSessionManager
    {
    public:
        explicit OnnxSessionManager(std::shared_ptr<UnifiedObservableConfigManager>) {}
        OnnxSessionLease borrowSession(const std::string &) { return OnnxSessionLease(nullptr, nullptr); }
        void clearCache() {}
        struct PoolStats
        {
            bool cache_enabled = false;
            size_t total_sessions = 0;
            size_t available_sessions = 0;
            size_t borrowed_sessions = 0;
            size_t max_sessions_per_model = 0;
        };
        PoolStats getStats() const { return {}; }
    };
#endif

} // namespace MediaDedup
