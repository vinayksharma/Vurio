// QW-7 (backend_optimization_plan.md): SQLite tuning pragmas are
// per-connection state, but they used to be applied to exactly ONE borrowed
// session at initialize() — the other pool connections ran SQLite defaults,
// most damagingly busy_timeout=0 (instant SQLITE_BUSY for concurrent writers,
// swallowed by the ops layer as silent write loss). The pool's
// customizeSession hook now applies them to every new physical connection.
// These tests hold several leases SIMULTANEOUSLY (forcing distinct physical
// connections) and assert each one reports the tuned values.

#include <gtest/gtest.h>
#include <filesystem>
#include <string>

#include <Poco/Data/Session.h>
#include <Poco/Data/Statement.h>

#include "database/database_manager.hpp"
#include "test_utils.hpp"

using namespace MediaDedup;
using Poco::Data::Keywords::into;
using Poco::Data::Keywords::now;

namespace
{
    int pragmaInt(Poco::Data::Session &sess, const std::string &pragma)
    {
        int value = -999;
        Poco::Data::Statement stmt(sess);
        stmt << "PRAGMA " + pragma, into(value), now;
        return value;
    }

    std::string pragmaText(Poco::Data::Session &sess, const std::string &pragma)
    {
        std::string value;
        Poco::Data::Statement stmt(sess);
        stmt << "PRAGMA " + pragma, into(value), now;
        return value;
    }
} // namespace

TEST(SessionPragmasTest, EveryConcurrentConnectionIsTuned)
{
    std::string db_path = MediaDedup::Test::TestUtils::getTestDataPath() + "/databases/test_session_pragmas.sqlite";
    std::filesystem::create_directories(std::filesystem::path(db_path).parent_path());
    std::filesystem::remove(db_path);

    DatabaseManager dbm(db_path);
    ASSERT_TRUE(dbm.initialize());

    // Hold 4 leases at once so the pool must hand out 4 DISTINCT physical
    // connections — the old code would have tuned at most one of them.
    // (Lease is intentionally non-movable, hence named locals.)
    auto l1 = dbm.acquireSessionLease();
    auto l2 = dbm.acquireSessionLease();
    auto l3 = dbm.acquireSessionLease();
    auto l4 = dbm.acquireSessionLease();

    auto expectTuned = [](Poco::Data::Session &sess, const char *which)
    {
        EXPECT_EQ(pragmaInt(sess, "busy_timeout"), 5000) << which << ": writers must wait, not fail with SQLITE_BUSY";
        EXPECT_EQ(pragmaInt(sess, "synchronous"), 1) << which << ": NORMAL expected (safe with WAL)";
        EXPECT_EQ(pragmaInt(sess, "foreign_keys"), 1) << which;
        EXPECT_EQ(pragmaInt(sess, "temp_store"), 2) << which << ": MEMORY expected";
        EXPECT_EQ(pragmaInt(sess, "cache_size"), 2000) << which << ": default page count when no config manager";
        EXPECT_EQ(pragmaText(sess, "journal_mode"), "wal") << which;
    };
    expectTuned(l1.get(), "lease1");
    expectTuned(l2.get(), "lease2");
    expectTuned(l3.get(), "lease3");
    expectTuned(l4.get(), "lease4");

    std::filesystem::remove(db_path);
}

TEST(SessionPragmasTest, ApplySqlitePragmasHonorsParameters)
{
    std::string db_path = MediaDedup::Test::TestUtils::getTestDataPath() + "/databases/test_session_pragmas_params.sqlite";
    std::filesystem::create_directories(std::filesystem::path(db_path).parent_path());
    std::filesystem::remove(db_path);

    DatabaseManager dbm(db_path);
    ASSERT_TRUE(dbm.initialize());
    auto lease = dbm.acquireSessionLease();
    Poco::Data::Session &sess = lease.get();

    SessionManager::applySqlitePragmas(sess, 1234, "file");
    EXPECT_EQ(pragmaInt(sess, "cache_size"), 1234);
    EXPECT_EQ(pragmaInt(sess, "temp_store"), 1) << "FILE expected";

    // Unknown temp_store value: leave whatever the connection already has.
    SessionManager::applySqlitePragmas(sess, 2000, "bogus");
    EXPECT_EQ(pragmaInt(sess, "temp_store"), 1) << "unknown value must not change temp_store";

    std::filesystem::remove(db_path);
}
