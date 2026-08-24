#include "core/server_initializer.hpp"
#include "config/unified_observable_config.hpp"
#include "utils/tool_paths.hpp"
#include "core/media_backfill.hpp"
#include "database/database_manager.hpp"
#include "database/database_service.hpp"
#include "database/user_settings_ops.hpp"
#include "database/user_settings_service.hpp"
#include "database/scanned_files_service.hpp"
#include "database/thumbnail_cache_ops.hpp"
#include "database/preview_cache_ops.hpp"
#include "database/ejected_files_ops.hpp"
#include "database/faces_ops.hpp"
#include "media_processors/face/face_clusterer.hpp"
#include "media_processors/face/face_scan_service.hpp"
#include "media_processors/face/vision_face_detector.hpp"
#include "filesmanager/files_service.hpp"
#include "filesmanager/disk_cache.hpp"
#include "orchestration/thread_pool_manager.hpp"
#include "orchestration/scheduler_service.hpp"
#include "orchestration/files_manager.hpp"
#include "orchestration/duplicate_finder.hpp"
#include "media_processors/media_processor.hpp"
#include "media_processors/image/backends/onnx_adapter.hpp"
#include <Poco/Logger.h>
#include <filesystem>
#include <thread>
#include <chrono>

namespace MediaDedup
{
    ServerInitializer::ServerInitializer(std::shared_ptr<UnifiedObservableConfigManager> config_manager)
        : config_manager_(std::move(config_manager))
    {
    }

    bool ServerInitializer::initializeConfiguration()
    {
        Poco::Logger &logger = Poco::Logger::get("ServerInitializer");

        try
        {
            // Initialize the configuration manager
            if (!config_manager_->initialize())
            {
                logger.error("Failed to initialize configuration manager");
                return false;
            }

            // Note: Thread stack size optimization is configured in ThreadPoolManager
            // HTTP server threads will use system default (8MB) - this is acceptable for HTTP processing
            // TPM threads will use optimized stack size (1MB) via tpm.thread.stackSizeKB setting

            // Publish the external ImageMagick binary location to the static
            // utilities (ThumbnailGenerator/transcoder) that have no config access.
            ToolPaths::setMagickPath(
                config_manager_->getPropertyValue<std::string>("tools.imagemagick.path", "/opt/homebrew/bin/magick"));

            // (MP-2: the thumbnail/preview backfill hook is now registered in
            // initializeMediaProcessing, where the real DiskCache instances
            // exist to be captured.)

            logger.information("Configuration initialized successfully");
            return true;
        }
        catch (const std::exception &e)
        {
            logger.error("Failed to load configuration: %s", e.what());
            return false;
        }
    }

    bool ServerInitializer::initializeDatabase()
    {
        Poco::Logger &logger = Poco::Logger::get("ServerInitializer");

        try
        {
            // Get database path from config
            std::string db_path = config_manager_->getPropertyValue<std::string>("database.path", "data/dedup_server.db");
            std::filesystem::path db_path_p(db_path);

            // Resolve relative paths against the config file's project root so the same DB is used
            // regardless of current working directory (e.g. run from vurio/ vs dedup_server/).
            if (!db_path_p.is_absolute())
            {
                std::filesystem::path config_path(config_manager_->getConfigFilePath());
                std::filesystem::path abs_config = std::filesystem::absolute(config_path);
                // Project root = directory containing the config folder (e.g. .../dedup_server)
                std::filesystem::path project_root = abs_config.parent_path().parent_path();
                db_path_p = (project_root / db_path_p).lexically_normal();
                db_path = db_path_p.string();
                logger.information("Database path resolved to: " + db_path);
            }

            // Create database directory if it doesn't exist
            std::filesystem::path db_dir = std::filesystem::path(db_path).parent_path();
            if (!db_dir.empty() && !std::filesystem::exists(db_dir))
            {
                std::filesystem::create_directories(db_dir);
            }

            // Initialize database manager with config manager for reactive pool sizing
            database_manager_ = std::make_shared<DatabaseManager>(db_path, config_manager_);
            if (!database_manager_->initialize())
            {
                logger.error("Failed to initialize database manager");
                return false;
            }

            // Initialize UserSettingsService
            user_settings_service_ = std::make_shared<UserSettingsService>(*database_manager_);
            if (!user_settings_service_->initialize())
            {
                logger.error("Failed to initialize user settings service");
                return false;
            }

            logger.information("Database initialized successfully");
            return true;
        }
        catch (const std::exception &e)
        {
            logger.error("Failed to initialize database: %s", e.what());
            return false;
        }
    }

    // AR-3: initializeWebServer() deleted with the legacy HTTP surface.

    bool ServerInitializer::initializeTPM()
    {
        Poco::Logger &logger = Poco::Logger::get("ServerInitializer");

        try
        {
            // Initialize TPM
            tpm_ = std::make_shared<ThreadPoolManager>(config_manager_);
            tpm_->initialize();

            logger.information("Thread Pool Manager initialized successfully");
            return true;
        }
        catch (const std::exception &e)
        {
            logger.error("Failed to initialize TPM: %s", e.what());
            return false;
        }
    }

    bool ServerInitializer::initializeSchedulerAndFiles()
    {
        Poco::Logger &logger = Poco::Logger::get("ServerInitializer");

        try
        {
            // Initialize SchedulerService
            scheduler_service_ = std::make_shared<Orchestration::SchedulerService>(config_manager_, tpm_);
            scheduler_service_->start();

            // Initialize thumbnail disk cache with separate configuration
            thumbnail_disk_cache_ = std::make_shared<DiskCache>(config_manager_, "cache.thumbnail");
            if (!thumbnail_disk_cache_->initialize())
            {
                logger.error("Failed to initialize thumbnail disk cache");
                return false;
            }
            logger.information("Thumbnail disk cache initialized successfully");

            // Initialize transcoding disk cache
            transcoding_disk_cache_ = std::make_shared<DiskCache>(config_manager_, "cache.disk");
            if (!transcoding_disk_cache_->initialize())
            {
                logger.error("Failed to initialize transcoding disk cache");
                return false;
            }
            logger.information("Transcoding disk cache initialized successfully");

            // Ensure thumbnail_cache database table exists
            if (!ThumbnailCacheOps::ensureTable(*database_manager_))
            {
                logger.error("Failed to ensure thumbnail_cache table");
                return false;
            }
            logger.information("Thumbnail cache table ensured");

            // Ensure preview_cache database table exists
            if (!PreviewCacheOps::ensureTable(*database_manager_))
            {
                logger.error("Failed to ensure preview_cache table");
                return false;
            }
            logger.information("Preview cache table ensured");

            // Clear cache tables if configured to clear physical caches on startup
            if (config_manager_->getPropertyValue<bool>("cache.thumbnail.clearOnStartup", false))
            {
                ThumbnailCacheOps::clearCache(*database_manager_);
            }
            if (config_manager_->getPropertyValue<bool>("cache.disk.clearOnStartup", false))
            {
                PreviewCacheOps::clearCache(*database_manager_);
            }

            // MP-2: DB-driven LRU eviction. Each cache evicts by its table's
            // last_accessed_at (real usage recency, not file mtime) and the
            // rows of evicted files are deleted in the same pass — dangling
            // rows previously caused regeneration storms.
            {
                auto db_for_hooks = database_manager_;
                thumbnail_disk_cache_->setEvictionHooks(
                    [db_for_hooks](std::size_t max_count)
                    { return ThumbnailCacheOps::lruCachedPaths(*db_for_hooks, static_cast<int>(max_count)); },
                    [db_for_hooks](const std::vector<std::string> &paths)
                    { ThumbnailCacheOps::deleteByCachedPaths(*db_for_hooks, paths); });
                transcoding_disk_cache_->setEvictionHooks(
                    [db_for_hooks](std::size_t max_count)
                    { return PreviewCacheOps::lruCachedPaths(*db_for_hooks, static_cast<int>(max_count)); },
                    [db_for_hooks](const std::vector<std::string> &paths)
                    { PreviewCacheOps::deleteByCachedPaths(*db_for_hooks, paths); });
            }

            // MP-3: purge poisoned size=0 rows — full-resolution renders that
            // leaked past validation, were cached forever, and could never be
            // served by the best-fit query. One-time cost proportional to the
            // damage; a clean install pays a single indexed query.
            {
                auto poisoned = ThumbnailCacheOps::purgeSizeZeroThumbnails(*database_manager_);
                for (const auto &p : poisoned)
                    thumbnail_disk_cache_->deleteFromCacheImmediately(p);
                if (!poisoned.empty())
                    logger.information("Purged %z size=0 full-resolution thumbnail(s)", poisoned.size());
            }

            // RAW-EXIF: register the integrity check's metadata sweep.
            MediaBackfill::installHook();

            // Ensure ejected_files_history database table exists
            if (!FacesOps::ensureTable(*database_manager_))
            {
                logger.error("Failed to ensure faces tables");
                return false;
            }
            logger.information("Faces tables ensured");

            if (!EjectedFilesOps::ensureTable(*database_manager_))
            {
                logger.error("Failed to ensure ejected_files_history table");
                return false;
            }
            logger.information("Ejected files history table ensured");

            // Initialize ONNX Session Manager (for QUALITY mode memory optimization)
            // Must be initialized before MediaProcessor for session reuse
            OnnxAdapter::initializeSessionManager(config_manager_);

            // Initialize MediaProcessor first (needed by FilesManager)
            auto db_shared = database_manager_; // CC-5: genuine shared ownership
            media_processor_ = std::make_shared<MediaProcessor>(config_manager_, db_shared, tpm_, thumbnail_disk_cache_, transcoding_disk_cache_);
            if (!media_processor_->initialize())
            {
                logger.error("Failed to initialize MediaProcessor");
                return false;
            }
            logger.information("MediaProcessor initialized successfully");

            // Initialize FilesManager with MediaProcessor
            files_service_ = std::make_shared<FilesService>(*db_shared, config_manager_);
            scanned_files_service_ = std::make_shared<ScannedFilesService>(*db_shared);
            files_manager_ = std::make_shared<Orchestration::FilesManager>(config_manager_, db_shared, files_service_, media_processor_);
            files_manager_->initialize();

            // Setup scan abort on path deregistration
            files_service_->setPathDeregisteredCallback([fm = files_manager_]()
                                                        {
                Poco::Logger::get("ServerInitializer").information("Path deregistered, aborting current scan if running");
                fm->abortCurrentScan(); });

            // Register fileScan job
            int intervalMs = config_manager_->getPropertyValue<int>("files.manager.scan.intervalMs", 300000);
            logger.information("Loading fileScan interval from config: %d ms", intervalMs);
            scheduler_service_->registerJob("fileScan", std::chrono::milliseconds(intervalMs), "fileScan",
                                            [fm = files_manager_]()
                                            { fm->runOnce(); });

            // Run an initial scan on startup so scan count reflects current directories without waiting for first interval
            if (scheduler_service_->triggerJob("fileScan"))
            {
                logger.information("Triggered initial file scan on startup");
            }
            else
            {
                logger.debug("Initial file scan trigger skipped (job already running or not ready)");
            }

            // Clear any processing flags from interrupted sessions before starting media processing
            logger.information("Clearing processing flags from any interrupted sessions...");
            int cleared_count = media_processor_->clearProcessingFlags();
            if (cleared_count > 0)
            {
                logger.information("Cleared processing flags for %d files that were left in processing state", cleared_count);
            }
            else if (cleared_count == 0)
            {
                logger.information("No files were left in processing state");
            }
            else
            {
                logger.warning("Failed to clear processing flags - database operation failed");
            }

            // Register mediaProcessor job
            int mediaIntervalMs = config_manager_->getPropertyValue<int>("media.processor.intervalMs", 30000);
            logger.information("Loading mediaProcessor interval from config: %d ms", mediaIntervalMs);
            // CC-6: was "fileScan" (copy-paste) — the media sweep shared the
            // scanner's per-type allowance. "media_processor" matches the
            // share MediaProcessor::initialize configures.
            scheduler_service_->registerJob("mediaProcessor", std::chrono::milliseconds(mediaIntervalMs), "media_processor",
                                            [mp = media_processor_]()
                                            { mp->ProcessMedia(); });

            // Initialize and register duplicate finder job
            duplicate_finder_ = std::shared_ptr<Orchestration::DuplicateFinder>(
                new Orchestration::DuplicateFinder(config_manager_, *database_manager_, tpm_));
            if (!duplicate_finder_->initialize())
            {
                logger.error("Failed to initialize DuplicateFinder");
                return false;
            }
            logger.information("DuplicateFinder initialized successfully");

            int dupIntervalMs = config_manager_->getPropertyValue<int>("duplicates.finder.intervalMs", 3600000);
            logger.information("Loading duplicateFinder interval from config: %d ms (%d hours)",
                               dupIntervalMs, dupIntervalMs / 3600000);
            scheduler_service_->registerJob("duplicateFinder", std::chrono::milliseconds(dupIntervalMs), "duplicate_finder",
                                            [df = duplicate_finder_]()
                                            { df->findDuplicates(); });

            // FACE-1: the trailing face sweep (docs/todo_face_recognition.md).
            // Off by default until the embedder model ships; with the flag on
            // but no model, faces are still detected and counted — embeddings
            // arrive on the next version bump once a model exists.
            if (config_manager_->getPropertyValue<bool>("media.face.enabled", false))
            {
                FaceScanService::Options face_opts;
                face_opts.batch = config_manager_->getPropertyValue<int>("media.face.sweepBatch", 200);
                std::string embed_model = config_manager_->getPropertyValue<std::string>(
                    "media.face.embedModelPath", "models/face-embed.onnx");
                std::filesystem::path embed_p(embed_model);
                if (!embed_p.is_absolute())
                {
                    std::filesystem::path cfg_p = std::filesystem::absolute(
                        std::filesystem::path(config_manager_->getConfigFilePath()));
                    embed_model = (cfg_p.parent_path().parent_path() / embed_p).lexically_normal().string();
                }
                auto embed_fn = [embed_model](const std::vector<float> &nchw, std::vector<float> &out) {
                    if (!std::filesystem::exists(embed_model))
                        return false;
                    return OnnxAdapter::ComputeFaceEmbedding(nchw, embed_model, 512, out);
                };
                FaceClusterer::Options cluster_opts;
                cluster_opts.join_threshold = config_manager_->getPropertyValue<double>("media.face.joinThreshold", 0.58);
                cluster_opts.merge_threshold = config_manager_->getPropertyValue<double>("media.face.mergeThreshold", 0.65);
                cluster_opts.min_quality = config_manager_->getPropertyValue<double>("media.face.minQuality", 0.2);
                auto clusterer = std::make_shared<FaceClusterer>(database_manager_.get(), cluster_opts);
                face_scan_service_ = std::make_shared<FaceScanService>(
                    database_manager_.get(), std::make_unique<VisionFaceDetector>(),
                    embed_fn, face_opts, clusterer);
                int face_interval = config_manager_->getPropertyValue<int>("media.face.sweepIntervalMs", 15000);
                scheduler_service_->registerJob("faceScan", std::chrono::milliseconds(face_interval), "face_scan",
                                                [fs = face_scan_service_]()
                                                { fs->runTick(); });
                logger.information("Face sweep registered: batch=%d, interval=%d ms, model=%s",
                                   face_opts.batch, face_interval, embed_model);

                // FACE-8: the purge cutoff adapts live — when the Settings
                // panel saves media.face.mergePurgeCutoff, latch a one-shot
                // purge sweep onto the face job's next tick (the RPC merge
                // paths read the new value directly; this covers the
                // already-clustered faces, the accepted re-binning).
                config_manager_->subscribeToConfigChanges(
                    [weakFace = std::weak_ptr<FaceScanService>(face_scan_service_),
                     cfg = config_manager_](const ConfigChangeEvent &event)
                    {
                        if (event.key != "media.face.mergePurgeCutoff")
                            return;
                        auto face = weakFace.lock();
                        if (!face)
                            return;
                        const double cutoff = cfg->getPropertyValue<double>(
                            "media.face.mergePurgeCutoff", FacesOps::kSuspectFitBar);
                        Poco::Logger::get("ServerInitializer")
                            .information("Face purge cutoff changed to %.2f — purge sweep queued", cutoff);
                        face->requestPurgeSweep(cutoff);
                    });
            }

            // Ensure in-flight work is aborted when the scheduler is paused
            // (e.g. during integrity checks). ORDERING IS LOAD-BEARING: the
            // lambda captures the shared_ptrs BY VALUE, so this must run after
            // BOTH files_manager_ and duplicate_finder_ are constructed —
            // setting it earlier captures a null duplicate_finder_ and a pause
            // can then never abort a running dedup (QW-3 in
            // docs/backend_optimization_plan.md).
            scheduler_service_->setPauseCallback([fm = files_manager_, df = duplicate_finder_]()
                                                 {
                                                     if (fm) fm->abortCurrentScan();
                                                     if (df) df->abortCurrentFind();
                                                 });

            // Subscribe to duplicate finder interval changes
            config_manager_->subscribeToConfigChanges([weakScheduler = std::weak_ptr<Orchestration::SchedulerService>(scheduler_service_),
                                                       cfg = config_manager_](const ConfigChangeEvent &event)
                                                      {
                if (event.key == "duplicates.finder.intervalMs")
                {
                    auto scheduler = weakScheduler.lock();
                    if (!scheduler)
                    {
                        return;
                    }

                    int newIntervalMs = cfg->getPropertyValue<int>("duplicates.finder.intervalMs", 3600000);
                    Poco::Logger &logger = Poco::Logger::get("ServerInitializer");
                    logger.information("Duplicate finder interval changed to: %d ms (%d hours)",
                                      newIntervalMs, newIntervalMs / 3600000);
                    
                    scheduler->updateJobInterval("duplicateFinder", std::chrono::milliseconds(newIntervalMs));
                } });

            // Set up immediate job triggering callback for FilesService
            files_service_->setPathRegisteredCallback([weakScheduler = std::weak_ptr<Orchestration::SchedulerService>(scheduler_service_),
                                                       cfg = config_manager_, this](const std::string &directory_path)
                                                      {
                Poco::Logger &logger = Poco::Logger::get("ServerInitializer");
                logger.debug("Path registered, triggering immediate scan and process: %s", directory_path);

                // Submit immediate scan task to thread pool instead of creating detached thread
                // This prevents thread accumulation and ensures proper lifecycle management
                if (tpm_)
                {
                    logger.debug("Submitting immediate scan task to thread pool for: %s", directory_path);
                    tpm_->submit("fileScan", [weakScheduler, cfg, directory_path]() {
                        Poco::Logger &threadLogger = Poco::Logger::get("ServerInitializer");
                        threadLogger.debug("Starting immediate scan task for: %s", directory_path);

                        auto scheduler = weakScheduler.lock();
                        if (!scheduler)
                        {
                            threadLogger.debug("Scheduler no longer available, skipping immediate scan and process");
                            return;
                        }

                        // Check if scheduler is still running before attempting to trigger jobs
                        if (!scheduler->isRunning()) {
                            threadLogger.debug("Scheduler is not running, skipping immediate scan and process");
                            return;
                        }

                        // Step 1: Trigger fileScan
                        bool scanTriggered = scheduler->triggerJob("fileScan");
                        if (!scanTriggered) {
                            threadLogger.debug("fileScan queued or already running");
                        }

                        // Step 2: Trigger mediaProcessor natively
                        bool processTriggered = scheduler->triggerJob("mediaProcessor");
                        if (processTriggered) {
                            threadLogger.debug("mediaProcessor job triggered successfully");
                        } else {
                            threadLogger.debug("mediaProcessor job was already running or not found");
                        }

                        threadLogger.debug("Immediate scan task completed for: %s", directory_path);
                    });
                    logger.debug("Immediate scan task submitted to thread pool for: %s", directory_path);
                }
                else
                {
                    logger.warning("ThreadPoolManager not available, skipping immediate scan for: %s", directory_path);
                } });

            logger.information("Scheduler and Files Manager initialized successfully");
            return true;
        }
        catch (const std::exception &e)
        {
            logger.error("Failed to initialize scheduler and files: %s", e.what());
            return false;
        }
    }

    void ServerInitializer::setupConfigChangeCallback(std::function<void(const std::string &)> callback)
    {
        config_manager_->setFileChangeCallback(callback);
    }

}
