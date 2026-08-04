#pragma once

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
} // namespace MediaDedup

namespace MediaDedup {
namespace Core {

class GrpcServerCore {
public:
    GrpcServerCore();
    ~GrpcServerCore();

    void start(const std::string& socket_path,
               std::shared_ptr<UnifiedObservableConfigManager> config_manager,
               std::shared_ptr<DatabaseManager> database_manager, // CC-5: real shared ownership
               std::shared_ptr<Orchestration::FilesManager> files_manager,
               std::shared_ptr<ThreadPoolManager> thread_pool_manager,
               std::shared_ptr<DiskCache> thumbnail_cache,
               std::shared_ptr<DiskCache> transcoding_cache,
               std::shared_ptr<Orchestration::DuplicateFinder> duplicate_finder,
               std::shared_ptr<Orchestration::SchedulerService> scheduler_service);
    void stop();

private:
    std::unique_ptr<grpc::Server> server_;
    // CC-5: keep-alive share for the raw DatabaseManager* the service
    // impls hold (they are members of services_ and die with us).
    std::shared_ptr<DatabaseManager> database_manager_;
    // CC-6: held so stop() can clear the group-event callback BEFORE
    // services_ teardown (the callback captures a raw service pointer).
    std::shared_ptr<Orchestration::DuplicateFinder> duplicate_finder_;
    std::string socket_path_;
    std::vector<std::unique_ptr<grpc::Service>> services_;
};

} // namespace Core
} // namespace MediaDedup
