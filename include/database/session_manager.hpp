#pragma once

#include <Poco/Data/Session.h>
#include <Poco/Data/SessionPool.h>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <memory>

namespace MediaDedup {

class SessionManager {
public:
    class Lease {
    public:
        explicit Lease(SessionManager &manager);
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
        Poco::Data::Session session_;
    };

    SessionManager(const std::string &connector,
                   const std::string &connection_string,
                   std::size_t pool_min = 1,
                   std::size_t pool_max = 8,
                   int idle_seconds = 60);

    bool initialize();

    Lease acquireLease();

    void setAcquireTimeoutMs(int ms) { acquire_timeout_ = std::chrono::milliseconds(ms); }
    void setBackoffMs(int ms) { acquire_backoff_ = std::chrono::milliseconds(ms); }

private:
    friend class Lease;

    std::string connector_;
    std::string conn_str_;
    std::unique_ptr<Poco::Data::SessionPool> pool_;

    // Pool sizing
    std::size_t pool_min_;
    std::size_t pool_max_;
    int idle_seconds_;

    // Lease/timeout control for bounded waits
    std::size_t leased_sessions_ = 0;
    std::mutex lease_mutex_;
    std::condition_variable lease_cv_;
    std::chrono::milliseconds acquire_timeout_{3000};
    std::chrono::milliseconds acquire_backoff_{50};

    Poco::Data::Session getConnectedSession();
    void releaseLease();
};

} // namespace MediaDedup


