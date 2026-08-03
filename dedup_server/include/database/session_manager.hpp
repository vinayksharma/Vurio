#pragma once

#include <Poco/Data/Session.h>
#include <Poco/Data/SessionPool.h>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <memory>
#include <optional>

namespace MediaDedup
{

    class SessionManager
    {
    public:
        class Lease
        {
        public:
            Lease(SessionManager &manager, Poco::Data::Session &&session);
            ~Lease();

            Poco::Data::Session &get();
            Poco::Data::Session *operator->() { return &get(); }
            Poco::Data::Session &operator*() { return get(); }

            Lease(const Lease &) = delete;
            Lease &operator=(const Lease &) = delete;
            Lease(Lease &&) = delete;
            Lease &operator=(Lease &&) = delete;

        private:
            SessionManager &manager_;
            // Optional so ~Lease can return the session to the Poco pool
            // BEFORE releasing the counted slot (DB-4): decrementing first
            // let another thread pass the counter gate while the pool was
            // still exhausted -> SessionPoolExhaustedException.
            std::optional<Poco::Data::Session> session_;
        };

        SessionManager(const std::string &connector,
                       const std::string &connection_string,
                       std::size_t pool_min = 1,
                       std::size_t pool_max = 8,
                       int idle_seconds = 60,
                       int sqlite_cache_size_pages = 2000,
                       std::string sqlite_temp_store = "memory");

        bool initialize();

        Lease acquireLease();

        /**
         * @brief Apply the per-connection SQLite tuning pragmas to one session.
         *
         * QW-7 (docs/backend_optimization_plan.md): these pragmas are
         * per-connection state, so they must run on EVERY physical connection
         * the pool creates (via customizeSession), not once at initialize().
         * Most critically `busy_timeout` — its default of 0 makes concurrent
         * writers fail instantly with SQLITE_BUSY, which the ops layer swallows
         * as `return false` (silent write loss under load).
         *
         * Public/static so tests can verify the exact pragma set.
         */
        static void applySqlitePragmas(Poco::Data::Session &session,
                                       int cache_size_pages,
                                       const std::string &temp_store);

        // Atomic: written by the config-monitor thread while workers read it
        // (DB-4). The old 50 ms sleep-poll backoff is gone — waiters block on
        // lease_cv_ and wake the moment a slot frees.
        void setAcquireTimeoutMs(int ms) { acquire_timeout_ms_.store(ms, std::memory_order_relaxed); }

    private:
        friend class Lease;

        std::string connector_;
        std::string conn_str_;
        std::unique_ptr<Poco::Data::SessionPool> pool_;

        // Pool sizing
        std::size_t pool_min_;
        std::size_t pool_max_;
        int idle_seconds_;

        // Per-connection SQLite tuning (applied by the pool's
        // customizeSession hook; see applySqlitePragmas)
        int sqlite_cache_size_pages_;
        std::string sqlite_temp_store_;

        // Lease/timeout control for bounded waits
        std::size_t leased_sessions_ = 0;
        std::mutex lease_mutex_;
        std::condition_variable lease_cv_;
        std::atomic<int> acquire_timeout_ms_{3000};

        Poco::Data::Session getConnectedSession();
        void releaseLease();
    };

} // namespace MediaDedup
