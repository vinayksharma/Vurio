// QW-7 (backend_optimization_plan.md): SQLite tuning pragmas are
// per-connection state, but they used to be applied to exactly ONE borrowed
// session at initialize() — the other pool connections ran SQLite defaults,
// most damagingly busy_timeout=0 (instant SQLITE_BUSY for concurrent writers,
// swallowed by the ops layer as silent write loss). The pool's
// customizeSession hook now applies them to every new physical connection.
// These tests hold several leases SIMULTANEOUSLY (forcing distinct physical
// connections) and assert each one reports the tuned values.

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "database/session_manager.hpp"

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

// ---- DB-4: acquire path (CV wait, slot/session ordering, timeout) ----

namespace
{
    std::string tempDb(const char *name)
    {
        auto p = std::filesystem::temp_directory_path() / name;
        std::filesystem::remove(p);
        return p.string();
    }
} // namespace

// A waiter blocked on an exhausted pool must wake promptly when the holder
// releases — the old implementation slept in 50 ms quanta; worse, the old
// ~Lease freed the counted slot BEFORE the session returned to the Poco pool,
// so the woken waiter could hit SessionPoolExhaustedException.
TEST(SessionAcquireTest, WaiterWakesOnReleaseAndGetsAValidSession)
{
    std::string db_path = tempDb("qw_db4_wake.sqlite");
    SessionManager sm("SQLite", db_path, 1, 1, 60);
    ASSERT_TRUE(sm.initialize());
    sm.setAcquireTimeoutMs(3000);

    std::atomic<bool> holder_has_lease{false};
    std::thread holder([&]() {
        auto lease = sm.acquireLease();
        holder_has_lease = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        // lease destroys here: session back to pool, then slot freed
    });

    while (!holder_has_lease)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    const auto t0 = std::chrono::steady_clock::now();
    {
        auto lease = sm.acquireLease(); // must block, then succeed
        // The session handed over after the wake must be usable immediately.
        int one = 0;
        Poco::Data::Statement stmt(lease.get());
        stmt << "SELECT 1", Poco::Data::Keywords::into(one), Poco::Data::Keywords::now;
        EXPECT_EQ(one, 1);
    }
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();

    holder.join();
    EXPECT_GE(waited, 100) << "should have blocked while the holder kept the lease";
    EXPECT_LT(waited, 1500) << "wake must be CV-driven, not timeout-driven";
    std::filesystem::remove(db_path);
}

TEST(SessionAcquireTest, TimesOutWhenPoolStaysExhausted)
{
    std::string db_path = tempDb("qw_db4_timeout.sqlite");
    SessionManager sm("SQLite", db_path, 1, 1, 60);
    ASSERT_TRUE(sm.initialize());
    sm.setAcquireTimeoutMs(200);

    auto held = sm.acquireLease();

    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_THROW(sm.acquireLease(), std::runtime_error);
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
    EXPECT_GE(waited, 180) << "must honor the configured timeout";
    EXPECT_LT(waited, 2000);
    std::filesystem::remove(db_path);
}

// Churn many threads through a 2-slot pool: every acquire must eventually
// succeed (no lost wakeups, no counter drift, no pool exhaustion from the
// release ordering).
TEST(SessionAcquireTest, ContendedChurnNeverLosesSlots)
{
    std::string db_path = tempDb("qw_db4_churn.sqlite");
    SessionManager sm("SQLite", db_path, 1, 2, 60);
    ASSERT_TRUE(sm.initialize());
    sm.setAcquireTimeoutMs(5000);

    constexpr int kThreads = 8;
    constexpr int kIterations = 50;
    std::atomic<int> successes{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t)
    {
        workers.emplace_back([&]() {
            for (int i = 0; i < kIterations; ++i)
            {
                auto lease = sm.acquireLease();
                int one = 0;
                Poco::Data::Statement stmt(lease.get());
                stmt << "SELECT 1", Poco::Data::Keywords::into(one), Poco::Data::Keywords::now;
                if (one == 1)
                    ++successes;
            }
        });
    }
    for (auto &w : workers)
        w.join();

    EXPECT_EQ(successes.load(), kThreads * kIterations);
    // Pool fully free again: two immediate acquires succeed back-to-back.
    {
        auto a = sm.acquireLease();
        auto b = sm.acquireLease();
    }
    std::filesystem::remove(db_path);
}
