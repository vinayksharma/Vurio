// BROKER-1: instance discovery — election on the well-known port,
// registration, busy propagation, and re-election when the broker leaves.
// Two InstanceBrokers in one process stand in for two server processes; a
// test port keeps this away from a real server running on the machine.

#include <gtest/gtest.h>

#include "core/broker/instance_broker.hpp"
#include "broker.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

using MediaDedup::Core::InstanceBroker;
using MediaDedup::Core::InstanceDescriptor;

namespace {

constexpr int kTestPort = 47999;

InstanceDescriptor makeDescriptor(const std::string &id, const std::string &commit, const std::string &group = "") {
    InstanceDescriptor d;
    d.instance_id = id;
    d.endpoint = "unix:/tmp/" + id + ".sock";
    d.cookie_path = "/tmp/" + id + ".cookie";
    d.version = "1.7.0";
    d.commit = commit;
    d.branch = "test";
    d.dirty = false;
    d.pid = 1000 + static_cast<int>(id.size());
    d.launch_group = group;
    return d;
}

std::vector<vurio::broker::Instance> listInstances(std::string *broker_id = nullptr) {
    auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(kTestPort), grpc::InsecureChannelCredentials());
    auto stub = vurio::broker::BrokerService::NewStub(channel);
    vurio::broker::ListInstancesRequest req;
    vurio::broker::ListInstancesResponse resp;
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
    grpc::Status st = stub->ListInstances(&ctx, req, &resp);
    if (!st.ok()) return {};
    if (broker_id) *broker_id = resp.broker_instance_id();
    return std::vector<vurio::broker::Instance>(resp.instances().begin(), resp.instances().end());
}

const vurio::broker::Instance *find(const std::vector<vurio::broker::Instance> &v, const std::string &id) {
    for (const auto &i : v)
        if (i.instance_id() == id) return &i;
    return nullptr;
}

// Poll until pred() holds or the budget runs out.
template <typename Pred>
bool eventually(Pred pred, std::chrono::milliseconds budget = std::chrono::seconds(8)) {
    auto until = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < until) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return pred();
}

} // namespace

TEST(InstanceBrokerTest, FirstWinsThePortAndOthersRegister) {
    InstanceBroker a(makeDescriptor("alpha", "aaaaaaaa", "/wt/a"), kTestPort);
    InstanceBroker b(makeDescriptor("beta", "bbbbbbbb"), kTestPort);
    a.start();
    b.start();

    EXPECT_TRUE(a.isBroker());
    EXPECT_FALSE(b.isBroker());

    std::string broker_id;
    ASSERT_TRUE(eventually([&] { return listInstances(&broker_id).size() == 2; }));
    EXPECT_EQ(broker_id, "alpha");

    auto all = listInstances();
    const auto *alpha = find(all, "alpha");
    const auto *beta = find(all, "beta");
    ASSERT_NE(alpha, nullptr);
    ASSERT_NE(beta, nullptr);
    EXPECT_EQ(alpha->launch_group(), "/wt/a");
    EXPECT_EQ(beta->commit(), "bbbbbbbb");
    EXPECT_EQ(beta->endpoint(), "unix:/tmp/beta.sock");
    EXPECT_FALSE(alpha->busy());
    EXPECT_FALSE(beta->busy());

    b.stop();
    a.stop();
}

TEST(InstanceBrokerTest, BusyPropagatesFromBothRoles) {
    InstanceBroker a(makeDescriptor("alpha", "aaaaaaaa"), kTestPort);
    InstanceBroker b(makeDescriptor("beta", "bbbbbbbb"), kTestPort);
    a.start();
    b.start();
    ASSERT_TRUE(eventually([&] { return listInstances().size() == 2; }));

    b.setBusy(true);
    EXPECT_TRUE(eventually([&] {
        auto v = listInstances();
        const auto *i = find(v, "beta");
        return i && i->busy();
    })) << "registrant busy flag must reach the broker";

    a.setBusy(true);
    EXPECT_TRUE(eventually([&] {
        auto v = listInstances();
        const auto *i = find(v, "alpha");
        return i && i->busy();
    })) << "broker's own busy flag must show in its registry";

    b.setBusy(false);
    EXPECT_TRUE(eventually([&] {
        auto v = listInstances();
        const auto *i = find(v, "beta");
        return i && !i->busy();
    }));

    b.stop();
    a.stop();
}

TEST(InstanceBrokerTest, RegistrantTakesOverWhenBrokerLeaves) {
    auto a = std::make_unique<InstanceBroker>(makeDescriptor("alpha", "aaaaaaaa"), kTestPort);
    InstanceBroker b(makeDescriptor("beta", "bbbbbbbb"), kTestPort);
    a->start();
    b.start();
    ASSERT_TRUE(a->isBroker());
    ASSERT_TRUE(eventually([&] { return listInstances().size() == 2; }));

    a->stop();
    a.reset();

    // beta notices on its next heartbeat and binds the port itself.
    std::string broker_id;
    ASSERT_TRUE(eventually([&] {
        auto v = listInstances(&broker_id);
        return b.isBroker() && v.size() == 1 && broker_id == "beta";
    })) << "the surviving registrant must become the broker";
    EXPECT_NE(find(listInstances(), "beta"), nullptr);

    // A newcomer registers with the new broker.
    InstanceBroker c(makeDescriptor("gamma", "cccccccc"), kTestPort);
    c.start();
    EXPECT_FALSE(c.isBroker());
    EXPECT_TRUE(eventually([&] { return find(listInstances(), "gamma") != nullptr; }));

    c.stop();
    EXPECT_TRUE(eventually([&] { return find(listInstances(), "gamma") == nullptr; }))
        << "a clean stop deregisters";
    b.stop();
}
