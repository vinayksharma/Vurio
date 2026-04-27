#include "core/grpc_server/grpc_server_core.hpp"
#include <grpcpp/grpcpp.h>
#include <Poco/Logger.h>
#include <filesystem>
#include "dedup.grpc.pb.h"
#include "config/unified_observable_config.hpp"
#include "database/database_manager.hpp"
#include "orchestration/files_manager.hpp"
#include <nlohmann/json.hpp>

namespace MediaDedup {
namespace Core {

class ConfigServiceImpl final : public vurio::dedup::ConfigService::Service {
    std::shared_ptr<UnifiedObservableConfigManager> config_;
public:
    explicit ConfigServiceImpl(std::shared_ptr<UnifiedObservableConfigManager> c) : config_(std::move(c)) {}
    grpc::Status GetConfig(grpc::ServerContext* context, const vurio::dedup::GetConfigRequest* request,
                           vurio::dedup::GetConfigResponse* reply) override {
        nlohmann::json json_config;
        for (const auto& key : config_->getAllPropertyKeys()) {
            auto property = config_->getProperty<int>(key); // Template parameter is unused in getProperty
            if (property) {
                json_config[key] = property->getValueAsString();
            }
        }
        reply->set_json_payload(json_config.dump(4));
        return grpc::Status::OK;
    }
};

class MediaLocationsServiceImpl final : public vurio::dedup::MediaLocationsService::Service {
    std::shared_ptr<FilesService> files_;
public:
    explicit MediaLocationsServiceImpl(std::shared_ptr<FilesService> f) : files_(std::move(f)) {}

    grpc::Status GetStatus(grpc::ServerContext* context, const vurio::dedup::GetStatusRequest* request,
                           vurio::dedup::GetStatusResponse* reply) override {
        auto locations = files_->listMediaLocations();
        for (const auto& pair : locations) {
            reply->add_registered_dirs(pair.second);
        }
        return grpc::Status::OK;
    }

    grpc::Status RegisterLocation(grpc::ServerContext* context, const vurio::dedup::RegisterLocationRequest* request,
                                  vurio::dedup::RegisterLocationResponse* reply) override {
        bool success = files_->registerMediaLocation(request->path());
        reply->set_success(success);
        if (!success) {
            reply->set_error_message("Failed to register location.");
        }
        return grpc::Status::OK;
    }

    grpc::Status DeregisterLocation(grpc::ServerContext* context, const vurio::dedup::DeregisterLocationRequest* request,
                                    vurio::dedup::DeregisterLocationResponse* reply) override {
        bool success = files_->deregisterMediaLocation(request->path());
        reply->set_success(success);
        if (!success) {
            reply->set_error_message("Failed to deregister location.");
        }
        return grpc::Status::OK;
    }

    grpc::Status ChangePath(grpc::ServerContext* context, const vurio::dedup::ChangePathRequest* request,
                            vurio::dedup::ChangePathResponse* reply) override {
        auto result = files_->changeMediaLocationPath(request->old_path(), request->new_path());
        reply->set_success(result.success);
        reply->set_error_message(result.error_message);
        reply->set_files_updated(result.files_updated);
        return grpc::Status::OK;
    }
};

class DuplicatesServiceImpl final : public vurio::dedup::DuplicatesService::Service {
    DatabaseManager* db_;
public:
    explicit DuplicatesServiceImpl(DatabaseManager* db) : db_(db) {}
};

GrpcServerCore::GrpcServerCore() {}

GrpcServerCore::~GrpcServerCore() {
    stop();
}

void GrpcServerCore::start(const std::string& socket_path,
                           std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                           DatabaseManager* database_manager,
                           std::shared_ptr<Orchestration::FilesManager> files_manager) {
    socket_path_ = socket_path;
    
    // Ensure socket directory exists
    std::filesystem::path path(socket_path_);
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }

    if (std::filesystem::exists(socket_path_)) {
        std::filesystem::remove(socket_path_);
    }

    std::string server_address("unix:" + socket_path_);
    
    auto config_service = std::make_unique<ConfigServiceImpl>(config_manager);
    auto media_service = std::make_unique<MediaLocationsServiceImpl>(files_manager->getFilesService());
    auto duplicates_service = std::make_unique<DuplicatesServiceImpl>(database_manager);

    grpc::ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(config_service.get());
    builder.RegisterService(media_service.get());
    builder.RegisterService(duplicates_service.get());
    
    services_.push_back(std::move(config_service));
    services_.push_back(std::move(media_service));
    services_.push_back(std::move(duplicates_service));

    server_ = builder.BuildAndStart();
    Poco::Logger::get("GrpcServerCore").information("gRPC Server listening on " + server_address);
}

void GrpcServerCore::stop() {
    if (server_) {
        server_->Shutdown();
        server_.reset();
        services_.clear();
        Poco::Logger::get("GrpcServerCore").information("gRPC Server stopped.");
    }
    if (!socket_path_.empty() && std::filesystem::exists(socket_path_)) {
        std::filesystem::remove(socket_path_);
    }
}

} // namespace Core
} // namespace MediaDedup
