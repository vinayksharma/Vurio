#include "core/broker/instance_broker.hpp"

#include "broker.grpc.pb.h"

#include <grpcpp/grpcpp.h>
#include <Poco/Logger.h>

#include <vector>

namespace MediaDedup {
namespace Core {

namespace {

Poco::Logger &logger() { return Poco::Logger::get("InstanceBroker"); }

vurio::broker::Instance toProto(const InstanceDescriptor &d, bool busy) {
    vurio::broker::Instance inst;
    inst.set_instance_id(d.instance_id);
    inst.set_endpoint(d.endpoint);
    inst.set_cookie_path(d.cookie_path);
    inst.set_version(d.version);
    inst.set_commit(d.commit);
    inst.set_branch(d.branch);
    inst.set_dirty(d.dirty);
    inst.set_busy(busy);
    inst.set_pid(d.pid);
    inst.set_launch_group(d.launch_group);
    return inst;
}

std::chrono::system_clock::time_point rpcDeadline() {
    return std::chrono::system_clock::now() + std::chrono::seconds(2);
}

} // namespace

struct InstanceBroker::Entry {
    vurio::broker::Instance instance;
    std::chrono::steady_clock::time_point last_seen;
};

// ---------------------------------------------------------------------------
// BrokerService: the registry, hosted by whichever instance won the port.
// ---------------------------------------------------------------------------
class InstanceBroker::BrokerServiceImpl final : public vurio::broker::BrokerService::Service {
public:
    explicit BrokerServiceImpl(InstanceBroker &owner) : owner_(owner) {}

    grpc::Status Register(grpc::ServerContext *, const vurio::broker::RegisterRequest *request,
                          vurio::broker::RegisterResponse *reply) override {
        const auto &inst = request->instance();
        if (inst.instance_id().empty() || inst.endpoint().empty()) {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "instance_id and endpoint are required");
        }
        {
            std::lock_guard<std::mutex> lk(owner_.registry_mutex_);
            auto entry = std::make_unique<Entry>();
            entry->instance = inst;
            entry->last_seen = std::chrono::steady_clock::now();
            owner_.registry_[inst.instance_id()] = std::move(entry);
        }
        logger().information("Registered instance %s (pid %s, %s %s, %s)",
                             inst.instance_id(), std::to_string(inst.pid()),
                             inst.version(), inst.commit(), inst.endpoint());
        reply->set_ok(true);
        return grpc::Status::OK;
    }

    grpc::Status Heartbeat(grpc::ServerContext *, const vurio::broker::HeartbeatRequest *request,
                           vurio::broker::HeartbeatResponse *reply) override {
        std::lock_guard<std::mutex> lk(owner_.registry_mutex_);
        auto it = owner_.registry_.find(request->instance_id());
        if (it == owner_.registry_.end()) {
            return grpc::Status(grpc::StatusCode::NOT_FOUND, "unknown instance; register first");
        }
        it->second->instance.set_busy(request->busy());
        it->second->last_seen = std::chrono::steady_clock::now();
        reply->set_ok(true);
        return grpc::Status::OK;
    }

    grpc::Status Deregister(grpc::ServerContext *, const vurio::broker::DeregisterRequest *request,
                            vurio::broker::DeregisterResponse *reply) override {
        {
            std::lock_guard<std::mutex> lk(owner_.registry_mutex_);
            owner_.registry_.erase(request->instance_id());
        }
        logger().information("Deregistered instance %s", request->instance_id());
        reply->set_ok(true);
        return grpc::Status::OK;
    }

    grpc::Status ListInstances(grpc::ServerContext *, const vurio::broker::ListInstancesRequest *,
                               vurio::broker::ListInstancesResponse *reply) override {
        owner_.pruneStale();
        std::lock_guard<std::mutex> lk(owner_.registry_mutex_);
        for (const auto &kv : owner_.registry_) {
            *reply->add_instances() = kv.second->instance;
        }
        reply->set_broker_instance_id(owner_.self_.instance_id);
        return grpc::Status::OK;
    }

private:
    InstanceBroker &owner_;
};

// ---------------------------------------------------------------------------
// InstanceBroker
// ---------------------------------------------------------------------------
InstanceBroker::InstanceBroker(InstanceDescriptor self, int port)
    : self_(std::move(self)), port_(port) {}

InstanceBroker::~InstanceBroker() { stop(); }

void InstanceBroker::start() {
    if (running_.exchange(true)) return;
    if (tryBecomeBroker()) {
        needs_register_ = false;
    } else if (registerWithBroker()) {
        needs_register_ = false;
    } else {
        logger().warning("No broker reachable on 127.0.0.1:%d and the port could not be bound; "
                         "will keep trying in the background", port_);
        resetClient();
    }
    agent_ = std::thread([this] { agentLoop(); });
}

void InstanceBroker::stop() {
    if (!running_.exchange(false)) return;
    {
        std::lock_guard<std::mutex> lk(wake_mutex_);
        wake_now_ = true;
    }
    wake_cv_.notify_all();
    if (agent_.joinable()) agent_.join();

    if (is_broker_.load()) {
        // Registrants notice on their next heartbeat and re-elect; nothing to
        // hand over, the registry is rebuilt from their re-registrations.
        closeBrokerServer();
    } else {
        deregisterFromBroker();
    }
    resetClient();
}

void InstanceBroker::setBusy(bool busy) {
    busy_.store(busy);
    {
        std::lock_guard<std::mutex> lk(wake_mutex_);
        wake_now_ = true;
    }
    wake_cv_.notify_all();
}

// --- election ---------------------------------------------------------------

bool InstanceBroker::tryBecomeBroker() {
    // A fresh service object per attempt: a grpc::Service may only ever be
    // registered with one ServerBuilder.
    auto service = std::make_unique<BrokerServiceImpl>(*this);

    grpc::ServerBuilder builder;
    // Election relies on the bind FAILING when the port is taken. gRPC sets
    // SO_REUSEPORT by default on POSIX, under which several processes bind
    // the same port happily and no one wins.
    builder.AddChannelArgument(GRPC_ARG_ALLOW_REUSEPORT, 0);
    int selected_port = 0;
    builder.AddListeningPort("127.0.0.1:" + std::to_string(port_),
                             grpc::InsecureServerCredentials(), &selected_port);
    builder.RegisterService(service.get());
    std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
    if (!server || selected_port == 0) {
        return false;
    }

    broker_service_ = std::move(service);
    broker_server_ = std::move(server);
    is_broker_.store(true);
    resetClient();
    {
        std::lock_guard<std::mutex> lk(registry_mutex_);
        registry_.clear();
    }
    upsertSelf();
    logger().information("This instance is the broker on 127.0.0.1:%d", port_);
    return true;
}

void InstanceBroker::closeBrokerServer() {
    if (broker_server_) {
        broker_server_->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(2));
        broker_server_.reset();
        broker_service_.reset();
        logger().information("Broker closed; another instance will take the role");
    }
    is_broker_.store(false);
}

// --- registrant side ----------------------------------------------------------

void InstanceBroker::resetClient() {
    client_channel_.reset();
}

bool InstanceBroker::registerWithBroker() {
    if (!client_channel_) {
        client_channel_ = grpc::CreateChannel("127.0.0.1:" + std::to_string(port_),
                                              grpc::InsecureChannelCredentials());
    }
    auto stub = vurio::broker::BrokerService::NewStub(client_channel_);
    vurio::broker::RegisterRequest req;
    *req.mutable_instance() = toProto(self_, busy_.load());
    vurio::broker::RegisterResponse resp;
    grpc::ClientContext ctx;
    ctx.set_deadline(rpcDeadline());
    grpc::Status st = stub->Register(&ctx, req, &resp);
    if (!st.ok() || !resp.ok()) {
        logger().debug("Register with broker failed: %s", st.error_message());
        return false;
    }
    needs_register_.store(false);
    logger().information("Registered with broker on 127.0.0.1:%d", port_);
    return true;
}

bool InstanceBroker::heartbeat() {
    if (!client_channel_) return false;
    auto stub = vurio::broker::BrokerService::NewStub(client_channel_);
    vurio::broker::HeartbeatRequest req;
    req.set_instance_id(self_.instance_id);
    req.set_busy(busy_.load());
    vurio::broker::HeartbeatResponse resp;
    grpc::ClientContext ctx;
    ctx.set_deadline(rpcDeadline());
    grpc::Status st = stub->Heartbeat(&ctx, req, &resp);
    if (st.ok()) return true;
    if (st.error_code() == grpc::StatusCode::NOT_FOUND) {
        // Broker is alive but has no entry for us (it is a new broker).
        needs_register_.store(true);
        return true;
    }
    logger().information("Broker unreachable (%s); re-electing", st.error_message());
    return false;
}

void InstanceBroker::deregisterFromBroker() {
    if (!client_channel_) return;
    auto stub = vurio::broker::BrokerService::NewStub(client_channel_);
    vurio::broker::DeregisterRequest req;
    req.set_instance_id(self_.instance_id);
    vurio::broker::DeregisterResponse resp;
    grpc::ClientContext ctx;
    ctx.set_deadline(rpcDeadline());
    stub->Deregister(&ctx, req, &resp);
}

// --- agent --------------------------------------------------------------------

void InstanceBroker::agentLoop() {
    while (running_.load()) {
        {
            std::unique_lock<std::mutex> lk(wake_mutex_);
            wake_cv_.wait_for(lk, kHeartbeatInterval, [this] { return wake_now_ || !running_.load(); });
            wake_now_ = false;
        }
        if (!running_.load()) break;
        try {
            tick();
        } catch (const std::exception &e) {
            logger().warning("Broker agent tick failed: %s", std::string(e.what()));
        }
    }
}

void InstanceBroker::tick() {
    if (is_broker_.load()) {
        pruneStale();
        upsertSelf();
        return;
    }
    if (needs_register_.load()) {
        if (registerWithBroker()) return;
        resetClient();
        if (tryBecomeBroker()) {
            needs_register_.store(false);
            return;
        }
        return; // no broker and no port: try again next tick
    }
    if (!heartbeat()) {
        resetClient();
        needs_register_.store(true);
        if (tryBecomeBroker()) {
            needs_register_.store(false);
        } else {
            registerWithBroker();
        }
    }
}

void InstanceBroker::pruneStale() {
    const auto now = std::chrono::steady_clock::now();
    std::vector<std::string> dropped;
    {
        std::lock_guard<std::mutex> lk(registry_mutex_);
        for (auto it = registry_.begin(); it != registry_.end();) {
            if (it->first != self_.instance_id && now - it->second->last_seen > kStaleAfter) {
                dropped.push_back(it->first);
                it = registry_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const auto &id : dropped) {
        logger().information("Dropped stale instance %s (no heartbeat for %ds)",
                             id, static_cast<int>(kStaleAfter.count()));
    }
}

void InstanceBroker::upsertSelf() {
    std::lock_guard<std::mutex> lk(registry_mutex_);
    auto &slot = registry_[self_.instance_id];
    if (!slot) slot = std::make_unique<Entry>();
    slot->instance = toProto(self_, busy_.load());
    slot->last_seen = std::chrono::steady_clock::now();
}

} // namespace Core
} // namespace MediaDedup
