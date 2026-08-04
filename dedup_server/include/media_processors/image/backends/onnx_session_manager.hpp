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
     * @brief Wrapper for an ONNX Runtime session
     *
     * DF-4: sessions share ONE process-wide Ort::Env (sharedEnv()) instead of
     * one env per session, and the model's I/O layout is introspected ONCE at
     * creation — the adapter previously re-queried input names/shapes/types
     * (with allocations) on every single inference.
     */
    struct OnnxSessionWrapper
    {
        std::unique_ptr<Ort::Session> session;
        std::string model_path;
        bool coreml_active = false;

        /// Model I/O layout, resolved once at session creation.
        struct IoSpec
        {
            size_t num_inputs = 0;
            std::vector<std::string> input_names;
            std::vector<const char *> input_name_ptrs; // stable views into input_names
            std::vector<std::vector<int64_t>> input_dims;
            std::vector<ONNXTensorElementDataType> input_types;
            size_t image_input_index = 0;
            bool found_image_input = false;
            bool image_input_nhwc = false;
            std::string output_name;
        };
        IoSpec io;

        /// The single process-wide ONNX Runtime environment.
        static Ort::Env &sharedEnv();

        /**
         * @param try_coreml append the CoreML execution provider; on ANY
         *        failure the session falls back to CPU-only (never throws
         *        for CoreML reasons alone)
         * @param coreml_cache_dir where CoreML stores compiled models so
         *        pooled sessions/restarts skip recompilation ("" = no cache)
         */
        OnnxSessionWrapper(const std::string &model_path_param,
                           bool try_coreml = false,
                           const std::string &coreml_cache_dir = "");
        ~OnnxSessionWrapper() = default;

        // Non-copyable, non-movable: io.input_name_ptrs points into
        // input_names, and wrappers always live behind unique_ptr anyway.
        OnnxSessionWrapper(const OnnxSessionWrapper &) = delete;
        OnnxSessionWrapper &operator=(const OnnxSessionWrapper &) = delete;
        OnnxSessionWrapper(OnnxSessionWrapper &&) = delete;
        OnnxSessionWrapper &operator=(OnnxSessionWrapper &&) = delete;

    private:
        void introspect();
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
        Ort::Env *getEnv() { return session_ ? &OnnxSessionWrapper::sharedEnv() : nullptr; }
        /// DF-4: full wrapper access for the cached I/O spec.
        OnnxSessionWrapper *getWrapper() { return session_; }
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
     * - media.image.onnx.sessionCache.enabled: Enable/disable caching
     * - media.image.onnx.sessionCache.maxSessions: Max sessions per model
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
        // DF-4: CoreML execution provider (read once at startup; sessions are
        // pooled, so flipping it live would give a mixed pool)
        bool coreml_enabled_ = false;
        std::string coreml_cache_dir_;

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
        void *getWrapper() { return nullptr; }
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
