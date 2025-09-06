#include "database/session_manager.hpp"
#include <Poco/Data/SessionPool.h>
#include <Poco/Data/Session.h>
#include <Poco/Data/SQLite/Connector.h>
#include <stdexcept>
#include <thread>

namespace MediaDedup {

SessionManager::SessionManager(const std::string &connector,
                               const std::string &connection_string,
                               std::size_t pool_min,
                               std::size_t pool_max,
                               int idle_seconds)
    : connector_(connector), conn_str_(connection_string), pool_min_(pool_min), pool_max_(pool_max), idle_seconds_(idle_seconds) {}

bool SessionManager::initialize()
{
    if (!pool_) {
        if (connector_ == "SQLite") {
            Poco::Data::SQLite::Connector::registerConnector();
        }
        pool_ = std::make_unique<Poco::Data::SessionPool>(connector_, conn_str_, pool_min_, pool_max_, idle_seconds_);
        // Smoke test
        Poco::Data::Session s = pool_->get();
        if (!s.isConnected()) return false;
    }
    return true;
}

SessionManager::Lease SessionManager::acquireLease()
{
    // Bounded wait on lease counter to avoid indefinite blocking
    auto deadline = std::chrono::steady_clock::now() + acquire_timeout_;
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(lease_mutex_);
            if (leased_sessions_ < pool_max_) { ++leased_sessions_; break; }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("Timed out acquiring DB session from pool");
        }
        std::this_thread::sleep_for(acquire_backoff_);
    }
    return Lease(*this);
}

Poco::Data::Session SessionManager::getConnectedSession()
{
    if (!pool_) throw std::runtime_error("Session pool not initialized");
    return pool_->get();
}

void SessionManager::releaseLease()
{
    std::lock_guard<std::mutex> lk(lease_mutex_);
    if (leased_sessions_ > 0) --leased_sessions_;
    lease_cv_.notify_one();
}

// Lease
SessionManager::Lease::Lease(SessionManager &manager)
    : manager_(manager), session_(manager.getConnectedSession()) {}

Poco::Data::Session &SessionManager::Lease::get() { return session_; }

SessionManager::Lease::~Lease() { manager_.releaseLease(); }

} // namespace MediaDedup


