#pragma once

#include <functional>
#include <string>
#include <memory>
#include <vector>

namespace grpc {
    class Server;
    class Service;
}

namespace MediaDedup {
class UnifiedObservableConfigManager;
class DatabaseManager;
class DiskCache;
namespace Orchestration {
    class FilesManager;
    class DuplicateFinder;
    class SchedulerService;
}
class ThreadPoolManager;
class TextSearchEngine;
} // namespace MediaDedup

namespace MediaDedup {
namespace Core {

class GrpcServerCore {
public:
    GrpcServerCore();
    ~GrpcServerCore();

    // BROKER-1: instance_dir is this process's private directory (created
    // 0700); the API socket and the pairing cookie live in it, so several
    // instances never share a path. on_pairing_changed(true/false) fires when
    // a client takes or drops the Session lock; on_client_gone fires after a
    // drop — the owner shuts the instance down (a server is single-use).
    void start(const std::string& instance_dir,
               std::shared_ptr<UnifiedObservableConfigManager> config_manager,
               std::shared_ptr<DatabaseManager> database_manager, // CC-5: real shared ownership
               std::shared_ptr<Orchestration::FilesManager> files_manager,
               std::shared_ptr<ThreadPoolManager> thread_pool_manager,
               std::shared_ptr<DiskCache> thumbnail_cache,
               std::shared_ptr<DiskCache> transcoding_cache,
               std::shared_ptr<Orchestration::DuplicateFinder> duplicate_finder,
               std::shared_ptr<Orchestration::SchedulerService> scheduler_service,
               std::function<void(bool)> on_pairing_changed = {},
               std::function<void()> on_client_gone = {});
    void stop();

    // gRPC channel target of this instance's API ("unix:<socket path>").
    const std::string& endpoint() const { return endpoint_; }
    const std::string& cookiePath() const { return cookie_path_; }

private:
    std::unique_ptr<grpc::Server> server_;
    // CC-5: keep-alive share for the raw DatabaseManager* the service
    // impls hold (they are members of services_ and die with us).
    std::shared_ptr<DatabaseManager> database_manager_;
    // CC-6: held so stop() can clear the group-event callback BEFORE
    // services_ teardown (the callback captures a raw service pointer).
    std::shared_ptr<Orchestration::DuplicateFinder> duplicate_finder_;
    std::string instance_dir_;
    std::string socket_path_;
    std::string cookie_path_;
    std::string endpoint_;
    // Session streams are cancelled by our own Shutdown too; this keeps that
    // from being reported as "the client left".
    std::shared_ptr<bool> stopping_;
    // Owned here (it is not a grpc::Service); the DuplicatesServiceImpl in
    // services_ holds a raw pointer, and services_ dies with us.
    std::shared_ptr<TextSearchEngine> text_search_engine_;
    std::vector<std::unique_ptr<grpc::Service>> services_;
};

} // namespace Core
} // namespace MediaDedup
