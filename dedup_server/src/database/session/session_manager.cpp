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

    SessionManager::SessionManager(const std::string &connector,
                                   const std::string &connection_string,
                                   std::size_t pool_min,
                                   std::size_t pool_max,
                                   int idle_seconds)
        : connector_(connector), conn_str_(connection_string), pool_min_(pool_min), pool_max_(pool_max), idle_seconds_(idle_seconds) {}

    bool SessionManager::initialize()
    {
        if (!pool_)
        {
            if (connector_ == "SQLite")
            {
                Poco::Data::SQLite::Connector::registerConnector();
            }
            pool_ = std::make_unique<Poco::Data::SessionPool>(connector_, conn_str_, pool_min_, pool_max_, idle_seconds_);
            Poco::Data::Session s = pool_->get();
            if (!s.isConnected())
                return false;

            // Configure SQLite for optimal multi-threaded performance
            if (connector_ == "SQLite")
            {
                try
                {
                    // Enable WAL mode for concurrent reads/writes
                    s << "PRAGMA journal_mode = WAL", Poco::Data::Keywords::now;
                    
                    // NORMAL synchronous mode is safe with WAL and much faster
                    s << "PRAGMA synchronous = NORMAL", Poco::Data::Keywords::now;
                    
                    // Increase cache size to 20MB for better performance
                    s << "PRAGMA cache_size = -20000", Poco::Data::Keywords::now;  // Negative = KB
                    
                    // Enable shared cache for better memory usage
                    s << "PRAGMA cache = shared", Poco::Data::Keywords::now;
                    
                    Poco::Logger::get("SessionManager").information("SQLite configured: WAL mode, synchronous=NORMAL, cache=20MB");
                }
                catch (const std::exception &e)
                {
                    Poco::Logger::get("SessionManager").error("Failed to configure SQLite pragmas: %s", std::string(e.what()));
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
