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
#include "core/web/web_server.hpp"
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

            // Register the thumbnail/preview backfill used by the database
            // integrity check (kept behind a hook so test binaries that compile
            // the integrity handler don't link the media-generation stack).
            MediaBackfill::installHook();

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
            database_manager_ = std::make_unique<DatabaseManager>(db_path, config_manager_);
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

    bool ServerInitializer::initializeWebServer()
    {
        Poco::Logger &logger = Poco::Logger::get("ServerInitializer");

        try
        {
            // Get host and port from config
            std::string host = config_manager_->getPropertyValue<std::string>("server.host", "0.0.0.0");
            int port_int = config_manager_->getPropertyValue<int>("server.port", 8080);
            uint16_t port = static_cast<uint16_t>(port_int);

            // Create web server with scheduler service
            web_server_ = std::make_unique<WebServer>(config_manager_, host, port, scheduler_service_);

            // Set up services
            if (user_settings_service_)
            {
                web_server_->setUserSettingsService(user_settings_service_);
                logger.information("UserSettingsService set on web server");
            }
            else
            {
                logger.warning("UserSettingsService is null - not setting on web server");
            }

            if (files_service_)
            {
                web_server_->setFilesService(files_service_);
                logger.information("FilesService set on web server");
            }
            else
            {
                logger.warning("FilesService is null - not setting on web server");
            }

            if (scanned_files_service_)
            {
                web_server_->setScannedFilesService(scanned_files_service_);
                logger.information("ScannedFilesService set on web server");
            }
            else
            {
                logger.warning("ScannedFilesService is null - not setting on web server");
            }

            if (tpm_)
            {
                web_server_->setThreadPoolManager(tpm_);
                logger.information("ThreadPoolManager set on web server");
            }
            else
            {
                logger.warning("ThreadPoolManager is null - not setting on web server");
            }

            // Set duplicate finder on web server for status endpoint
            if (duplicate_finder_)
            {
                web_server_->setDuplicateFinder(duplicate_finder_);
                logger.information("DuplicateFinder set on web server");
            }
            else
            {
                logger.warning("DuplicateFinder is null - not setting on web server");
            }

            // Set database manager and thumbnail cache on web server for thumbnail endpoint
            auto db_shared = std::shared_ptr<DatabaseManager>(database_manager_.get(), [](DatabaseManager *) {});
            if (db_shared)
            {
                web_server_->setDatabaseManager(db_shared);
                logger.information("DatabaseManager set on web server");
            }

            if (thumbnail_disk_cache_)
            {
                web_server_->setThumbnailCache(thumbnail_disk_cache_);
                logger.information("ThumbnailCache set on web server");
            }
            else
            {
                logger.warning("ThumbnailCache is null - not setting on web server");
            }

            if (transcoding_disk_cache_)
            {
                web_server_->setTranscodingCache(transcoding_disk_cache_);
                logger.information("TranscodingCache set on web server for thumbnail API");
            }
            else
            {
                logger.warning("Transcoding cache is null - not setting on web server");
            }

            // Start the web server
            if (!web_server_->start())
            {
                logger.error("Failed to start web server");
                return false;
            }

            logger.information("Web server started on %s:%u", host, static_cast<unsigned int>(port));
            return true;
        }
        catch (const std::exception &e)
        {
            logger.error("Failed to initialize web server: %s", e.what());
            return false;
        }
    }

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

            // Ensure ejected_files_history database table exists
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
            auto db_shared = std::shared_ptr<DatabaseManager>(database_manager_.get(), [](DatabaseManager *) {});
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

            // Ensure scans are aborted when scheduler is paused (e.g., during integrity checks)
            scheduler_service_->setPauseCallback([fm = files_manager_, df = duplicate_finder_]()
                                                 {
                                                     if (fm) fm->abortCurrentScan();
                                                     if (df) df->abortCurrentFind();
                                                 });

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
            scheduler_service_->registerJob("mediaProcessor", std::chrono::milliseconds(mediaIntervalMs), "fileScan",
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

    void ServerInitializer::setupRequestHandlers()
    {
        // This would be implemented based on the existing web server setup
        // For now, we'll leave it as a placeholder since the web server
        // handles its own request handler setup
    }
}
