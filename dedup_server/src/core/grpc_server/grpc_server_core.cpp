/*
 * AR-5: grpc_server_core.cpp is now ONLY server lifecycle (start/stop) —
 * pairing-secret publication, builder wiring, auth processor registration,
 * socket permissions, shutdown. The six service implementations live in
 * include/core/grpc_server/{security,config,media_locations,duplicates,
 * advanced_settings,media}_service.hpp as constructible, testable classes.
 */
#include "core/grpc_server/grpc_server_core.hpp"

#include "core/grpc_server/grpc_auth.hpp"
#include "core/grpc_server/security_service.hpp"
#include "core/grpc_server/config_service.hpp"
#include "core/grpc_server/media_locations_service.hpp"
#include "core/grpc_server/duplicates_service.hpp"
#include "core/grpc_server/advanced_settings_service.hpp"
#include "core/grpc_server/media_service.hpp"
#include "search/text_search_engine.hpp"

#include <grpcpp/grpcpp.h>
#include <Poco/Logger.h>
#include <filesystem>
#include <fstream>
#include <cstdlib>

namespace MediaDedup {
namespace Core {

GrpcServerCore::GrpcServerCore() {}

GrpcServerCore::~GrpcServerCore() {
    stop();
}

void GrpcServerCore::start(const std::string& socket_path,
                           std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                           std::shared_ptr<DatabaseManager> database_manager,
                           std::shared_ptr<Orchestration::FilesManager> files_manager,
                           std::shared_ptr<ThreadPoolManager> thread_pool_manager,
                           std::shared_ptr<DiskCache> thumbnail_cache,
                           std::shared_ptr<DiskCache> transcoding_cache,
                           std::shared_ptr<Orchestration::DuplicateFinder> duplicate_finder,
                           std::shared_ptr<Orchestration::SchedulerService> scheduler_service) {
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

    // Generate this instance's pairing secrets and publish the cookie to a 0600
    // file only the same OS user can read. Done before BuildAndStart so the
    // token is in place before any call is accepted.
    {
        std::string cookie = GrpcAuth::generateSecretHex();
        std::string token = GrpcAuth::generateSecretHex();
        {
            std::lock_guard<std::mutex> lk(GrpcAuth::authState().mutex);
            GrpcAuth::authState().pairing_cookie = cookie;
            GrpcAuth::authState().session_token = token;
        }
        const char* home = std::getenv("HOME");
        if (home && *home) {
            std::error_code ec;
            std::filesystem::path dir = std::filesystem::path(home) / ".vurio";
            std::filesystem::create_directories(dir, ec);
            std::filesystem::permissions(dir, std::filesystem::perms::owner_all,
                                         std::filesystem::perm_options::replace, ec);
            std::filesystem::path cookie_file = dir / "grpc.cookie";
            { std::ofstream ofs(cookie_file, std::ios::trunc); ofs << cookie; }
            std::filesystem::permissions(cookie_file,
                std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                std::filesystem::perm_options::replace, ec);
            Poco::Logger::get("GrpcServerCore").information("Wrote gRPC pairing cookie to " + cookie_file.string());
        } else {
            Poco::Logger::get("GrpcServerCore").error("HOME not set; gRPC clients will not be able to pair");
        }
    }

    database_manager_ = database_manager; // CC-5 keep-alive for raw db_ members
    auto security_service = std::make_unique<SecurityServiceImpl>();
    auto config_service = std::make_unique<ConfigServiceImpl>(config_manager);
    std::shared_ptr<MediaDedup::FilesService> files_service = nullptr;
    if (files_manager) {
        files_service = files_manager->getFilesService();
    }
    auto media_service = std::make_unique<MediaLocationsServiceImpl>(
        files_service, thread_pool_manager, database_manager.get(), scheduler_service, duplicate_finder,
        config_manager && config_manager->getPropertyValue<bool>("media.face.enabled", false));
    text_search_engine_ = std::make_shared<TextSearchEngine>(config_manager, database_manager.get());
    auto duplicates_service = std::make_unique<DuplicatesServiceImpl>(database_manager.get(), text_search_engine_.get(), config_manager);
    std::unique_ptr<vurio::dedup::AdvancedSettingsService::Service> advanced_settings_service = std::make_unique<AdvancedSettingsServiceImpl>(database_manager, config_manager, thumbnail_cache, transcoding_cache, scheduler_service, thread_pool_manager);
    auto media_streaming_service = std::make_unique<MediaServiceImpl>(config_manager, database_manager, thumbnail_cache, transcoding_cache, thread_pool_manager);

    duplicate_finder_ = duplicate_finder; // CC-6: for callback teardown in stop()
    if (duplicate_finder) {
        // We use a raw pointer to the service because it's managed by services_ below;
        // stop() clears this callback before services_ teardown (CC-6).
        DuplicatesServiceImpl* dup_svc = duplicates_service.get();
        duplicate_finder->setGroupEventCallback([dup_svc](int group_id, bool is_new) {
            dup_svc->OnDuplicateGroupEvent(group_id, is_new);
        });
        duplicate_finder->setGroupsRebuiltCallback([dup_svc]() {
            dup_svc->OnGroupsRebuilt();
        });
    }

    grpc::ServerBuilder builder;
    // AR-4: the auth processor is attached to the server credentials so the
    // session check runs before ANY handler, for every present and future RPC.
    // Local (UDS) credentials, not insecure ones: identical wire format (no
    // TLS), but they carry a real security context, which is what makes
    // SetAuthMetadataProcessor work — on InsecureServerCredentials gRPC
    // aborts with "Not Supported" (verified empirically on gRPC 1.80).
    auto server_creds = grpc::experimental::LocalServerCredentials(UDS);
    server_creds->SetAuthMetadataProcessor(std::make_shared<GrpcAuth::SessionAuthProcessor>());
    builder.AddListeningPort(server_address, server_creds);
    // AR-4: bound the sync server's worker threads — previously unbounded,
    // so a thumbnail-scroll burst could spawn threads without limit.
    grpc::ResourceQuota quota("vurio-grpc");
    quota.SetMaxThreads(32);
    builder.SetResourceQuota(quota);
    builder.RegisterService(security_service.get());
    builder.RegisterService(config_service.get());
    builder.RegisterService(media_service.get());
    builder.RegisterService(duplicates_service.get());
    builder.RegisterService(advanced_settings_service.get());
    builder.RegisterService(media_streaming_service.get());
    
    services_.push_back(std::move(security_service));
    services_.push_back(std::move(config_service));
    services_.push_back(std::move(media_service));
    services_.push_back(std::move(duplicates_service));
    services_.push_back(std::move(advanced_settings_service));
    services_.push_back(std::move(media_streaming_service));

    server_ = builder.BuildAndStart();

    // Restrict the socket to the owner so only the same OS user can connect.
    {
        std::error_code ec;
        std::filesystem::permissions(socket_path_,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace, ec);
    }

    Poco::Logger::get("GrpcServerCore").information("gRPC Server listening on " + server_address);
}

void GrpcServerCore::stop() {
    // CC-6: detach the finder's callbacks BEFORE tearing down the service they
    // point at — a still-running dedup task could otherwise call into freed
    // memory during the shutdown drain window.
    if (duplicate_finder_) {
        duplicate_finder_->setGroupEventCallback(nullptr);
        duplicate_finder_->setGroupsRebuiltCallback(nullptr);
        duplicate_finder_.reset();
    }
    if (server_) {
        // CC-6: deadline — an un-deadlined Shutdown blocks until every RPC
        // returns; the 1 s-polling streams alone stall it, and a stuck unary
        // handler hangs shutdown forever. In-flight RPCs get 5 s, then are
        // cancelled.
        server_->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(5));
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
