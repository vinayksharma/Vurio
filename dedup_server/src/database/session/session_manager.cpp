#include "database/session_manager.hpp"
#include <Poco/Data/Session.h>
#include <Poco/Data/SessionPool.h>
#include <Poco/Data/Session.h>
#include <Poco/Data/SQLite/Connector.h>
#include <Poco/Logger.h>
#include <stdexcept>
#include <thread>

namespace MediaDedup
{
    namespace
    {
        /// SessionPool whose customizeSession hook applies the per-connection
        /// SQLite pragmas the moment a new physical connection is created.
        /// QW-7: previously the pragmas ran on exactly one borrowed session at
        /// initialize(), so the other pool connections ran SQLite defaults —
        /// most damagingly busy_timeout=0 (instant SQLITE_BUSY for concurrent
        /// writers, swallowed by the ops layer as silent write loss).
        class SqliteTunedSessionPool : public Poco::Data::SessionPool
        {
        public:
            SqliteTunedSessionPool(const std::string &connector,
                                   const std::string &connection_string,
                                   int min_sessions,
                                   int max_sessions,
                                   int idle_time,
                                   int cache_size_pages,
                                   std::string temp_store)
                : Poco::Data::SessionPool(connector, connection_string, min_sessions, max_sessions, idle_time),
                  cache_size_pages_(cache_size_pages), temp_store_(std::move(temp_store)) {}

        protected:
            void customizeSession(Poco::Data::Session &session) override
            {
                SessionManager::applySqlitePragmas(session, cache_size_pages_, temp_store_);
            }

        private:
            int cache_size_pages_;
            std::string temp_store_;
        };
    }

    SessionManager::SessionManager(const std::string &connector,
                                   const std::string &connection_string,
                                   std::size_t pool_min,
                                   std::size_t pool_max,
                                   int idle_seconds,
                                   int sqlite_cache_size_pages,
                                   std::string sqlite_temp_store)
        : connector_(connector), conn_str_(connection_string), pool_min_(pool_min), pool_max_(pool_max),
          idle_seconds_(idle_seconds), sqlite_cache_size_pages_(sqlite_cache_size_pages),
          sqlite_temp_store_(std::move(sqlite_temp_store)) {}

    void SessionManager::applySqlitePragmas(Poco::Data::Session &session,
                                            int cache_size_pages,
                                            const std::string &temp_store)
    {
        try
        {
            // Writers wait for the lock instead of failing instantly.
            session << "PRAGMA busy_timeout = 5000", Poco::Data::Keywords::now;
            // Safe with WAL (set persistently at initialize) and much faster
            // than the FULL default.
            session << "PRAGMA synchronous = NORMAL", Poco::Data::Keywords::now;
            session << "PRAGMA cache_size = " << std::to_string(cache_size_pages), Poco::Data::Keywords::now;
            if (temp_store == "memory")
                session << "PRAGMA temp_store = MEMORY", Poco::Data::Keywords::now;
            else if (temp_store == "file")
                session << "PRAGMA temp_store = FILE", Poco::Data::Keywords::now;
            session << "PRAGMA foreign_keys = ON", Poco::Data::Keywords::now;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("SessionManager").error("Failed to apply per-connection SQLite pragmas: %s", std::string(e.what()));
            // Don't fail - the connection still works with default settings
        }
    }

    bool SessionManager::initialize()
    {
        if (!pool_)
        {
            if (connector_ == "SQLite")
            {
                Poco::Data::SQLite::Connector::registerConnector();
                pool_ = std::make_unique<SqliteTunedSessionPool>(connector_, conn_str_,
                                                                 static_cast<int>(pool_min_),
                                                                 static_cast<int>(pool_max_),
                                                                 idle_seconds_,
                                                                 sqlite_cache_size_pages_,
                                                                 sqlite_temp_store_);
            }
            else
            {
                pool_ = std::make_unique<Poco::Data::SessionPool>(connector_, conn_str_,
                                                                  static_cast<int>(pool_min_),
                                                                  static_cast<int>(pool_max_),
                                                                  idle_seconds_);
            }
            Poco::Data::Session s = pool_->get();
            if (!s.isConnected())
                return false;

            if (connector_ == "SQLite")
            {
                try
                {
                    // WAL is a property of the database FILE (persistent), so
                    // once here is correct; the per-connection pragmas are
                    // applied by customizeSession on every new connection.
                    s << "PRAGMA journal_mode = WAL", Poco::Data::Keywords::now;
                    Poco::Logger::get("SessionManager").information(
                        "SQLite configured: WAL journal; per-connection pragmas (busy_timeout=5000, synchronous=NORMAL, cache_size=%d pages, temp_store=%s, foreign_keys=ON)",
                        sqlite_cache_size_pages_, sqlite_temp_store_);
                }
                catch (const std::exception &e)
                {
                    Poco::Logger::get("SessionManager").error("Failed to configure SQLite journal mode: %s", std::string(e.what()));
                    // Don't fail initialization - database will work with default settings
                }
            }
        }
        return true;
    }

    // (Removed stray free function)

    SessionManager::Lease SessionManager::acquireLease()
    {
        // Bounded wait on lease counter to avoid indefinite blocking
        auto deadline = std::chrono::steady_clock::now() + acquire_timeout_;
        for (;;)
        {
            {
                std::unique_lock<std::mutex> lk(lease_mutex_);
                if (leased_sessions_ < pool_max_)
                {
                    ++leased_sessions_;
                    try
                    {
                        // Try to get the session while holding the lock
                        Poco::Data::Session session = getConnectedSession();
                        lk.unlock(); // Release lock before creating Lease
                        return Lease(*this, std::move(session));
                    }
                    catch (...)
                    {
                        // If session creation fails, decrement the counter
                        --leased_sessions_;
                        throw;
                    }
                }
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                throw std::runtime_error("Timed out acquiring DB session from pool");
            }
            std::this_thread::sleep_for(acquire_backoff_);
        }
    }

    Poco::Data::Session SessionManager::getConnectedSession()
    {
        if (!pool_)
            throw std::runtime_error("Session pool not initialized");
        // Per-connection pragmas (incl. foreign_keys) are applied once per
        // physical connection by the pool's customizeSession hook — no
        // per-checkout statement round trip here (QW-7).
        return pool_->get();
    }

    void SessionManager::releaseLease()
    {
        std::lock_guard<std::mutex> lk(lease_mutex_);
        if (leased_sessions_ > 0)
            --leased_sessions_;
        lease_cv_.notify_one();
    }

    // Lease
    SessionManager::Lease::Lease(SessionManager &manager)
        : manager_(manager), session_(manager.getConnectedSession()) {}

    SessionManager::Lease::Lease(SessionManager &manager, Poco::Data::Session &&session)
        : manager_(manager), session_(std::move(session)) {}

    Poco::Data::Session &SessionManager::Lease::get() { return session_; }

    SessionManager::Lease::~Lease() { manager_.releaseLease(); }

} // namespace MediaDedup
