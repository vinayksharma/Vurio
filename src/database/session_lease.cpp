#include "database/session_lease.hpp"
#include "database/database_manager.hpp"

namespace MediaDedup
{

    SessionLease::SessionLease(DatabaseManager &manager)
        : manager_(manager), session_(manager.acquireSessionLease().get())
    {
    }

    Poco::Data::Session &SessionLease::get()
    {
        return session_;
    }

    SessionLease::~SessionLease()
    {
        // no-op after SessionManager migration
    }

} // namespace MediaDedup
