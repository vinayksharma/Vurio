#include "core/media_ready_bus.hpp"
#include "database/faces_ops.hpp"
#include "adjustments/mask_service.hpp"
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
#include "core/grpc_server/adjustments_service.hpp"
#include "core/grpc_server/preview_service.hpp"
#include "core/grpc_server/recipes_service.hpp"
#include "adjustments/adjustment_store.hpp"
#include "adjustments/chain_engine.hpp"
#include "database/preview_cache_ops.hpp"
#include "media_processors/image/backends/transcoding_pipeline.hpp"
#include "utils/thumbnail_generator.hpp"
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

void GrpcServerCore::start(const std::string& instance_dir,
                           std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                           std::shared_ptr<DatabaseManager> database_manager,
                           std::shared_ptr<Orchestration::FilesManager> files_manager,
                           std::shared_ptr<ThreadPoolManager> thread_pool_manager,
                           std::shared_ptr<DiskCache> thumbnail_cache,
                           std::shared_ptr<DiskCache> transcoding_cache,
                           std::shared_ptr<Orchestration::DuplicateFinder> duplicate_finder,
                           std::shared_ptr<Orchestration::SchedulerService> scheduler_service,
                           std::function<void(bool)> on_pairing_changed,
                           std::function<void()> on_client_gone) {
    instance_dir_ = instance_dir;
    socket_path_ = (std::filesystem::path(instance_dir_) / "grpc.sock").string();
    cookie_path_ = (std::filesystem::path(instance_dir_) / "grpc.cookie").string();
    endpoint_ = "unix:" + socket_path_;
    stopping_ = std::make_shared<bool>(false);

    // BROKER-1: the instance directory is ours alone (it is keyed by pid), so
    // anything in it is a leftover of a previous process with the same pid.
    {
        std::error_code ec;
        std::filesystem::create_directories(instance_dir_, ec);
        std::filesystem::permissions(instance_dir_, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace, ec);
        std::filesystem::remove(socket_path_, ec);
    }

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
            GrpcAuth::authState().session_held = false;
            GrpcAuth::authState().session_holder.clear();
            GrpcAuth::authState().shutting_down.store(false);
        }
        std::error_code ec;
        { std::ofstream ofs(cookie_path_, std::ios::trunc); ofs << cookie; }
        std::filesystem::permissions(cookie_path_,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace, ec);
        Poco::Logger::get("GrpcServerCore").information("Wrote gRPC pairing cookie to " + cookie_path_);
    }

    database_manager_ = database_manager; // CC-5 keep-alive for raw db_ members
    // The session hooks run on the gRPC handler thread of the Session stream.
    // A stream cancelled by our own stop() must not look like a departed
    // client, or shutdown would re-enter shutdown.
    std::shared_ptr<bool> stopping = stopping_;
    auto security_service = std::make_unique<SecurityServiceImpl>(
        [] {},
        [on_pairing_changed] {
            if (on_pairing_changed) on_pairing_changed(true);
        },
        [on_pairing_changed, on_client_gone, stopping] {
            if (*stopping) return;
            if (on_pairing_changed) on_pairing_changed(false);
            if (on_client_gone) on_client_gone();
        });
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
    auto adjustments_service = std::make_unique<AdjustmentsServiceImpl>(); // ADJ-1
    // CHAIN-1 (docs/todo_tool_chain.md §5.4): live preview sessions. The
    // base loader reuses the existing decode paths: the QHD preview when
    // the format needs one, the original (scaled decode) otherwise.
    auto preview_service = std::make_unique<PreviewServiceImpl>();
    auto recipes_service = std::make_unique<RecipesServiceImpl>(); // CHAIN-3
    {
        auto cfg = config_manager;
        auto db_shared = database_manager;
        MediaDedup::Adjustments::PreviewSessionManager::instance().configure(
            cfg ? cfg->getPropertyValue<int>("media.adjustments.preview.sessions", 3) : 3,
            static_cast<size_t>(cfg ? cfg->getPropertyValue<int>("media.adjustments.preview.cacheBytes", 402653184) : 402653184),
            cfg ? cfg->getPropertyValue<int>("media.adjustments.preview.idleSeconds", 120) : 120,
            cfg ? cfg->getPropertyValue<int>("media.adjustments.preview.sessionSide", 2560) : 2560,
            [cfg, db_shared](const std::string& file_path, int max_side, MediaDedup::Adjustments::Size& base_full) -> cv::Mat {
                base_full = {};
                if (db_shared) {
                    if (auto row = MediaDedup::ScannedFilesOps::getByPath(*db_shared, file_path); row && row->exif_resolution) {
                        auto dims = MediaDedup::displayDimensionsFromExifResolution(*row->exif_resolution);
                        base_full = {static_cast<double>(dims.width), static_cast<double>(dims.height)};
                    }
                }
                if (!TranscodingPipeline::NeedsQhdPreviewCache(file_path, cfg))
                    return MediaDedup::Adjustments::ChainEngine::decodeBgr8(file_path, max_side);
                if (db_shared) {
                    if (auto cached = MediaDedup::PreviewCacheOps::getPreview(*db_shared, file_path);
                        cached && std::filesystem::exists(cached->cached_path)) {
                        cv::Mat img = MediaDedup::Adjustments::ChainEngine::decodeBgr8(cached->cached_path, max_side);
                        if (!img.empty()) return img;
                    }
                }
                // No QHD preview yet: a 1024 px identity render stands in
                // until the preview exists (the viewer requests it anyway).
                Magick::Blob blob;
                if (!ThumbnailGenerator::generateToBlob(file_path, 1024, blob, 90)) return {};
                std::vector<uint8_t> bytes(static_cast<const uint8_t*>(blob.data()),
                                           static_cast<const uint8_t*>(blob.data()) + blob.length());
                return MediaDedup::Adjustments::ChainEngine::decodeBgr8(bytes);
            },
            [](const std::string& file_path) {
                return MediaDedup::Adjustments::AdjustmentStore::instance().documentJsonFor(file_path);
            });

        // CHAIN-5 (docs §5.6): detected masks — the same base decode, the
        // library's face boxes, a "mask" event when a detection lands.
        {
            using MediaDedup::Adjustments::MaskService;
            MaskService::Config mc;
            mc.enabled = cfg ? cfg->getPropertyValue<bool>("media.adjustments.masks.enabled", true) : true;
            mc.size = cfg ? cfg->getPropertyValue<int>("media.adjustments.masks.size", 1024) : 1024;
            std::string cache_root = cfg ? cfg->getPropertyValue<std::string>("cache.disk.location", "data/cache.previews") : "data/cache.previews";
            mc.cache_dir = (std::filesystem::path(cache_root) / "masks").string();
            mc.models.prompt_model = cfg ? cfg->getPropertyValue<std::string>("media.adjustments.masks.promptModelPath", "models/clipseg-rd64-refined-q8.onnx") : "models/clipseg-rd64-refined-q8.onnx";
            mc.models.people_model = cfg ? cfg->getPropertyValue<std::string>("media.adjustments.masks.peopleModelPath", "models/selfie-segmentation.onnx") : "models/selfie-segmentation.onnx";
            mc.models.use_coreml = cfg ? cfg->getPropertyValue<bool>("media.adjustments.masks.useCoreML", false) : false;
            std::filesystem::path model_dir = std::filesystem::path(mc.models.prompt_model).parent_path();
            if (model_dir.empty()) model_dir = "models";
            mc.models.vocab_json = (model_dir / "vocab.json").string();
            mc.models.merges_txt = (model_dir / "merges.txt").string();
            MaskService::FaceLookup faces;
            if (db_shared) {
                faces = [db_shared](const std::string& file_path) {
                    std::vector<MediaDedup::Adjustments::MaskDetectors::FaceBox> boxes;
                    auto row = MediaDedup::ScannedFilesOps::getByPath(*db_shared, file_path);
                    if (!row) return boxes;
                    for (const auto& f : MediaDedup::FacesOps::listFacesForFile(*db_shared, row->id)) {
                        MediaDedup::Adjustments::MaskDetectors::FaceBox b;
                        b.x = f.bbox_x; b.y = f.bbox_y; b.w = f.bbox_w; b.h = f.bbox_h;
                        b.person_id = f.person_id; b.cluster_id = f.cluster_id;
                        boxes.push_back(b);
                    }
                    return boxes;
                };
            }
            MaskService::instance().configure(
                mc,
                [](const std::string& file_path, int max_side, MediaDedup::Adjustments::Size& base_full) {
                    return MediaDedup::Adjustments::PreviewSessionManager::instance().loadBase(file_path, max_side, base_full);
                },
                faces,
                [](const std::string& file_path) {
                    MediaDedup::MediaReadyBus::Event ev;
                    ev.file_path = file_path;
                    ev.media_type = "mask";
                    ev.success = true;
                    MediaDedup::MediaReadyBus::instance().publish(ev);
                });
        }
    }

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
    builder.AddListeningPort(endpoint_, server_creds);
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
    builder.RegisterService(adjustments_service.get()); // ADJ-1
    builder.RegisterService(preview_service.get()); // CHAIN-1
    builder.RegisterService(recipes_service.get()); // CHAIN-3
    
    services_.push_back(std::move(security_service));
    services_.push_back(std::move(config_service));
    services_.push_back(std::move(media_service));
    services_.push_back(std::move(duplicates_service));
    services_.push_back(std::move(advanced_settings_service));
    services_.push_back(std::move(media_streaming_service));
    services_.push_back(std::move(adjustments_service));
    services_.push_back(std::move(preview_service));
    services_.push_back(std::move(recipes_service));

    server_ = builder.BuildAndStart();

    // Restrict the socket to the owner so only the same OS user can connect.
    {
        std::error_code ec;
        std::filesystem::permissions(socket_path_,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace, ec);
    }

    Poco::Logger::get("GrpcServerCore").information("gRPC Server listening on " + endpoint_);
}

void GrpcServerCore::stop() {
    if (stopping_) *stopping_ = true;
    GrpcAuth::authState().shutting_down.store(true);
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
        // CHAIN-1: the preview session manager is process-wide and its base
        // loader captured the database and config handles at start(); drop
        // them with the server, or a later start() (and every test fixture
        // that reopens a database at the same path) finds the old one still
        // held open.
        MediaDedup::Adjustments::PreviewSessionManager::instance().reset();
        MediaDedup::Adjustments::MaskService::instance().reset(); // CHAIN-5: stop the detector worker
        Poco::Logger::get("GrpcServerCore").information("gRPC Server stopped.");
    }
    if (!instance_dir_.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(instance_dir_, ec);
        instance_dir_.clear();
    }
}
} // namespace Core
} // namespace MediaDedup
