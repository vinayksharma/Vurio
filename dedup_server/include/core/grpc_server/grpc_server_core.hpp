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
namespace Orchestration {
    class FilesManager;
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
               DatabaseManager* database_manager,
               std::shared_ptr<Orchestration::FilesManager> files_manager,
               std::shared_ptr<ThreadPoolManager> thread_pool_manager);
    void stop();

private:
    std::unique_ptr<grpc::Server> server_;
    std::string socket_path_;
    std::vector<std::unique_ptr<grpc::Service>> services_;
};

} // namespace Core
} // namespace MediaDedup
