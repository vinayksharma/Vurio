#include "database/session_lease.hpp"
#include "database/database_manager.hpp"

namespace MediaDedup
{

    SessionLease::SessionLease(DatabaseManager &manager)
        : manager_(manager), session_(manager.getConnectedSession())
    {
    }

    Poco::Data::Session &SessionLease::get()
    {
        return session_;
    }

    SessionLease::~SessionLease()
    {
        manager_.releaseLease();
    }

} // namespace MediaDedup
