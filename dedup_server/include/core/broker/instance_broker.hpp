#pragma once

// BROKER-1: instance discovery for concurrent server+UI pairs.
//
// Every server process owns one InstanceBroker. At start it tries to bind the
// well-known broker endpoint (127.0.0.1:<port>). Whoever wins hosts the
// registry (BrokerService, see broker.proto) and lists itself in it; everyone
// else registers with the winner and heartbeats every couple of seconds. A
// registrant whose broker disappears retries the bind on its next tick, so
// the role moves on its own when the broker's process exits — a UI's
// discovery simply sees no broker for a moment and asks again.
//
// The registry is a plain in-memory map, never persisted: a crashed entry
// falls out after the stale window because nobody heartbeats it.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace grpc {
class Server;
class Channel;
class Service;
}

namespace MediaDedup {
namespace Core {

struct InstanceDescriptor {
    std::string instance_id;
    std::string endpoint;      // e.g. "unix:/Users/me/.vurio/instances/123/grpc.sock"
    std::string cookie_path;
    std::string version;
    std::string commit;
    std::string branch;
    bool dirty = false;
    std::int64_t pid = 0;
    std::string launch_group;  // VURIO_INSTANCE_GROUP, or empty
};

class InstanceBroker {
public:
    // Fixed on both sides; the UI reads VURIO_BROKER_PORT, the server
    // server.broker.port. Chosen well clear of IANA-registered and ephemeral
    // ranges' common defaults.
    static constexpr int kDefaultPort = 47911;

    static constexpr std::chrono::seconds kHeartbeatInterval{2};
    static constexpr std::chrono::seconds kStaleAfter{10};

    InstanceBroker(InstanceDescriptor self, int port = kDefaultPort);
    ~InstanceBroker();

    InstanceBroker(const InstanceBroker &) = delete;
    InstanceBroker &operator=(const InstanceBroker &) = delete;

    // Election + registration, then the agent thread. Never throws: an
    // instance that cannot reach any broker keeps retrying in the background
    // and still serves a UI that finds it by other means.
    void start();
    // Deregister (or hand the broker role over by closing it), stop the
    // agent. Idempotent.
    void stop();

    // Pairing lock state, pushed to the broker on the next heartbeat (and
    // immediately when it changes).
    void setBusy(bool busy);

    bool isBroker() const { return is_broker_.load(); }
    int port() const { return port_; }
    const InstanceDescriptor &self() const { return self_; }

private:
    class BrokerServiceImpl;
    friend class BrokerServiceImpl;

    struct Entry;

    bool tryBecomeBroker();
    void closeBrokerServer();
    bool registerWithBroker();
    bool heartbeat();
    void deregisterFromBroker();
    void resetClient();
    void agentLoop();
    void tick();
    void pruneStale();
    void upsertSelf();

    InstanceDescriptor self_;
    int port_;

    std::atomic<bool> is_broker_{false};
    std::atomic<bool> busy_{false};
    std::atomic<bool> running_{false};
    // Set when the broker rejected our last heartbeat (or we never
    // registered) so the next tick re-registers.
    std::atomic<bool> needs_register_{true};

    std::mutex registry_mutex_;
    std::map<std::string, std::unique_ptr<Entry>> registry_;

    std::unique_ptr<grpc::Service> broker_service_;
    std::unique_ptr<grpc::Server> broker_server_;
    std::shared_ptr<grpc::Channel> client_channel_;

    std::mutex wake_mutex_;
    std::condition_variable wake_cv_;
    bool wake_now_ = false;
    std::thread agent_;
};

} // namespace Core
} // namespace MediaDedup
