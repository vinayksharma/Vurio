#pragma once

#include <Poco/Data/Session.h>

namespace MediaDedup
{

    class DatabaseManager;

    class SessionLease
    {
    public:
        explicit SessionLease(DatabaseManager &manager);
        ~SessionLease();

        Poco::Data::Session &get();
        Poco::Data::Session *operator->() { return &get(); }
        Poco::Data::Session &operator*() { return get(); }

        SessionLease(const SessionLease &) = delete;
        SessionLease &operator=(const SessionLease &) = delete;
        SessionLease(SessionLease &&) = delete;
        SessionLease &operator=(SessionLease &&) = delete;

    private:
        DatabaseManager &manager_;
        Poco::Data::Session session_;
    };

} // namespace MediaDedup
