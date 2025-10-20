#include "config/config_manager_factory.hpp"
#include <cstdlib>
#include <iostream>

namespace MediaDedup
{

    std::shared_ptr<UnifiedObservableConfigManager> ConfigManagerFactory::createDefault(
        const std::string &config_file_path)
    {
        ConfigManagerConfig config;
        config.config_file_path = config_file_path;
        return createWithConfig(config);
    }

    std::shared_ptr<UnifiedObservableConfigManager> ConfigManagerFactory::createWithConfig(
        const ConfigManagerConfig &config)
    {
        if (!validateConfig(config))
        {
            throw std::invalid_argument("Invalid configuration provided to ConfigManagerFactory");
        }

        auto manager = std::shared_ptr<UnifiedObservableConfigManager>(
            createInstance(config),
            [](UnifiedObservableConfigManager *ptr)
            { delete ptr; });

        configureInstance(manager.get(), config);
        return manager;
    }

    std::shared_ptr<UnifiedObservableConfigManager> ConfigManagerFactory::createForTesting(
        const std::string &config_file_path)
    {
        ConfigManagerConfig config;
        config.config_file_path = config_file_path.empty() ? "test_config.yaml" : config_file_path;
        config.enable_file_monitoring = false;
        config.enable_validation = true;
        config.auto_save = false;
        config.enable_events = true;
        config.emit_file_change_events = false;
        config.emit_programmatic_events = true;
        config.strict_validation = false;
        config.validate_on_load = true;
        config.validate_on_save = false;

        return createWithConfig(config);
    }

    std::shared_ptr<UnifiedObservableConfigManager> ConfigManagerFactory::createForProduction(
        const std::string &config_file_path,
        bool enable_monitoring)
    {
        ConfigManagerConfig config;
        config.config_file_path = config_file_path;
        config.enable_file_monitoring = enable_monitoring;
        config.enable_validation = true;
        config.auto_save = true;
        config.enable_events = true;
        config.emit_file_change_events = true;
        config.emit_programmatic_events = true;
        config.strict_validation = true;
        config.validate_on_load = true;
        config.validate_on_save = true;
        config.reload_interval = std::chrono::milliseconds(2000);
        config.file_check_interval = std::chrono::milliseconds(1000);

        return createWithConfig(config);
    }

    std::shared_ptr<UnifiedObservableConfigManager> ConfigManagerFactory::createForDevelopment(
        const std::string &config_file_path)
    {
        ConfigManagerConfig config;
        config.config_file_path = config_file_path;
        config.enable_file_monitoring = true;
        config.enable_validation = true;
        config.auto_save = true;
        config.enable_events = true;
        config.emit_file_change_events = true;
        config.emit_programmatic_events = true;
        config.strict_validation = false;
        config.validate_on_load = true;
        config.validate_on_save = true;
        config.reload_interval = std::chrono::milliseconds(500);
        config.file_check_interval = std::chrono::milliseconds(250);

        return createWithConfig(config);
    }

    std::shared_ptr<UnifiedObservableConfigManager> ConfigManagerFactory::createWithCustomComponents(
        const ConfigManagerConfig &config,
        std::function<std::unique_ptr<ConfigFileManager>(const std::string &)> file_manager_factory,
        std::function<std::unique_ptr<ConfigValidator>()> validator_factory,
        std::function<std::unique_ptr<ConfigEventManager>()> event_manager_factory)
    {
        if (!validateConfig(config))
        {
            throw std::invalid_argument("Invalid configuration provided to ConfigManagerFactory");
        }

        // For now, we'll use the standard factory methods
        // In a more advanced implementation, we could inject custom components
        return createWithConfig(config);
    }

    ConfigManagerConfig ConfigManagerFactory::getDefaultConfig(const std::string &environment)
    {
        ConfigManagerConfig config;

        if (environment == "development")
        {
            config.enable_file_monitoring = true;
            config.enable_validation = true;
            config.auto_save = true;
            config.enable_events = true;
            config.emit_file_change_events = true;
            config.emit_programmatic_events = true;
            config.strict_validation = false;
            config.validate_on_load = true;
            config.validate_on_save = true;
            config.reload_interval = std::chrono::milliseconds(500);
            config.file_check_interval = std::chrono::milliseconds(250);
        }
        else if (environment == "testing")
        {
            config.enable_file_monitoring = false;
            config.enable_validation = true;
            config.auto_save = false;
            config.enable_events = true;
            config.emit_file_change_events = false;
            config.emit_programmatic_events = true;
            config.strict_validation = false;
            config.validate_on_load = true;
            config.validate_on_save = false;
        }
        else if (environment == "production")
        {
            config.enable_file_monitoring = true;
            config.enable_validation = true;
            config.auto_save = true;
            config.enable_events = true;
            config.emit_file_change_events = true;
            config.emit_programmatic_events = true;
            config.strict_validation = true;
            config.validate_on_load = true;
            config.validate_on_save = true;
            config.reload_interval = std::chrono::milliseconds(2000);
            config.file_check_interval = std::chrono::milliseconds(1000);
        }
        else
        {
            // Default configuration
            config.enable_file_monitoring = true;
            config.enable_validation = true;
            config.auto_save = true;
            config.enable_events = true;
            config.emit_file_change_events = true;
            config.emit_programmatic_events = true;
            config.strict_validation = false;
            config.validate_on_load = true;
            config.validate_on_save = true;
        }

        return config;
    }

    bool ConfigManagerFactory::validateConfig(const ConfigManagerConfig &config)
    {
        if (config.config_file_path.empty())
        {
            return false;
        }

        if (config.reload_interval.count() < 100)
        {
            return false;
        }

        if (config.file_check_interval.count() < 50)
        {
            return false;
        }

        return true;
    }

    std::shared_ptr<UnifiedObservableConfigManager> ConfigManagerFactory::createFromEnvironment(
        const std::string &config_file_path)
    {
        ConfigManagerConfig config;
        config.config_file_path = config_file_path;

        // Read from environment variables
        const char *env_monitoring = std::getenv("CONFIG_ENABLE_MONITORING");
        if (env_monitoring)
        {
            config.enable_file_monitoring = (std::string(env_monitoring) == "true" || std::string(env_monitoring) == "1");
        }

        const char *env_validation = std::getenv("CONFIG_ENABLE_VALIDATION");
        if (env_validation)
        {
            config.enable_validation = (std::string(env_validation) == "true" || std::string(env_validation) == "1");
        }

        const char *env_auto_save = std::getenv("CONFIG_AUTO_SAVE");
        if (env_auto_save)
        {
            config.auto_save = (std::string(env_auto_save) == "true" || std::string(env_auto_save) == "1");
        }

        const char *env_strict = std::getenv("CONFIG_STRICT_VALIDATION");
        if (env_strict)
        {
            config.strict_validation = (std::string(env_strict) == "true" || std::string(env_strict) == "1");
        }

        const char *env_reload_interval = std::getenv("CONFIG_RELOAD_INTERVAL");
        if (env_reload_interval)
        {
            try
            {
                int interval = std::stoi(env_reload_interval);
                config.reload_interval = std::chrono::milliseconds(interval);
            }
            catch (const std::exception &)
            {
                // Use default value
            }
        }

        const char *env_log_level = std::getenv("CONFIG_LOG_LEVEL");
        if (env_log_level)
        {
            config.log_level = env_log_level;
        }

        return createWithConfig(config);
    }

    UnifiedObservableConfigManager *ConfigManagerFactory::createInstance(const ConfigManagerConfig &config)
    {
        return new UnifiedObservableConfigManager(
            config.config_file_path,
            config.enable_file_monitoring,
            config.reload_interval);
    }

    void ConfigManagerFactory::configureInstance(UnifiedObservableConfigManager *manager, const ConfigManagerConfig &config)
    {
        if (!manager)
        {
            return;
        }

        // Configure validation
        if (config.enable_validation)
        {
            manager->setValidationEnabled(true);
        }
        else
        {
            manager->setValidationEnabled(false);
        }

        // Set up default properties
        setupDefaultProperties(manager, config);

        // Set up validation callbacks
        if (config.enable_validation)
        {
            setupValidationCallbacks(manager, config);
        }

        // Set up event callbacks
        if (config.enable_events)
        {
            setupEventCallbacks(manager, config);
        }

        // Initialize the manager
        if (!manager->initialize())
        {
            throw std::runtime_error("Failed to initialize UnifiedObservableConfigManager");
        }
    }

    void ConfigManagerFactory::setupDefaultProperties(UnifiedObservableConfigManager *manager, const ConfigManagerConfig &config)
    {
        if (!manager)
        {
            return;
        }

        // Create default server properties (matching CONFIGURATION_REFERENCE.md)
        manager->createProperty<std::string>("server.host", "0.0.0.0", "Server host address");
        manager->createProperty<int>("server.port", 8080, "Server port number");
        manager->createProperty<std::string>("server.name", "Media Deduplication Server", "Server name");
        manager->createProperty<std::string>("server.mode", "FAST", "Server mode (FAST|BALANCED|QUALITY)");
        manager->createProperty<std::string>("server.processName", "media_dedup_server", "Process name for instance checking");
        manager->createProperty<bool>("server.instanceCheck.enabled", true, "Enable instance checking");
        manager->createProperty<int>("server.instanceCheck.bufferSize", 128, "Instance check buffer size");
        manager->createProperty<int>("server.max_connections", 100, "Maximum number of connections");
        manager->createProperty<double>("server.timeout", 30.0, "Server timeout in seconds");

        // Create default database properties
        manager->createProperty<std::string>("database.path", "data/dedup_server.db", "Database file path");
        manager->createProperty<int>("database.session.acquireTimeoutMs", 3000, "Database session acquire timeout");
        manager->createProperty<int>("database.session.acquireBackoffMs", 50, "Database session acquire backoff");
        manager->createProperty<int>("database.session.poolMin", 4, "Minimum database session pool size");
        manager->createProperty<int>("database.session.poolMax", 20, "Maximum database session pool size");

        // Create default logging properties
        manager->createProperty<std::string>("logging.level", config.log_level, "Logging level");
        manager->createProperty<bool>("logging.enable_console", true, "Enable console logging");
        manager->createProperty<bool>("logging.enable_file", false, "Enable file logging");

        // Create default files manager properties
        manager->createProperty<bool>("files.manager.enabled", true, "Enable files manager");
        manager->createProperty<int>("files.manager.scan.intervalMs", 500, "File scan interval in milliseconds");

        // Create default scheduler properties
        manager->createProperty<bool>("scheduler.jitter.enabled", false, "Enable scheduler jitter");
        manager->createProperty<int>("scheduler.jitter.percent", 0, "Scheduler jitter percentage");
        manager->createProperty<std::string>("scheduler.drift.mode", "anchored", "Scheduler drift mode");
        manager->createProperty<int>("scheduler.drift.maxDriftMs", 60000, "Maximum scheduler drift");
        manager->createProperty<bool>("scheduler.backoff.enabled", true, "Enable scheduler backoff");
        manager->createProperty<int>("scheduler.backoff.initialMs", 1000, "Initial scheduler backoff");
        manager->createProperty<double>("scheduler.backoff.multiplier", 2.0, "Scheduler backoff multiplier");
        manager->createProperty<int>("scheduler.backoff.maxMs", 30000, "Maximum scheduler backoff");
        manager->createProperty<int>("scheduler.backoff.jitterPercent", 10, "Scheduler backoff jitter percentage");

        // Create default TPM properties
        manager->createProperty<std::string>("tpm.pool.max", "auto", "TPM pool maximum size");
        manager->createProperty<int>("tpm.killTimeoutMs", 10000, "TPM kill timeout");
        manager->createProperty<int>("tpm.thread.idleTimeoutSeconds", 120, "Thread idle timeout in seconds");
        manager->createProperty<double>("tpm.types.fileScan.share", 1.0, "File scan type share");

        // Create default media processor properties
        manager->createProperty<bool>("media.processor.enabled", true, "Enable media processor");
        manager->createProperty<int>("media.processor.intervalMs", 30000, "Media processor interval in milliseconds");

        // TPM thread type shares for media processing (consolidated naming: tpm.types.<type>.share)
        manager->createProperty<double>("tpm.types.media_processor.share", 1.0, "Thread pool share for media processor tasks");
        manager->createProperty<double>("tpm.types.image_processor.share", 1.0, "Reserved: Thread pool share for image processor");
        manager->createProperty<double>("tpm.types.audio_processor.share", 1.0, "Reserved: Thread pool share for audio processor");
        manager->createProperty<double>("tpm.types.video_processor.share", 1.0, "Reserved: Thread pool share for video processor");

        // Image pipelines SAFE TREO configuration
        manager->createProperty<int>("media.image.timeoutMs", 30000, "Per-image processing timeout in milliseconds");
        manager->createProperty<bool>("media.image.retry.enabled", true, "Enable retries for transient errors");
        manager->createProperty<int>("media.image.retry.maxAttempts", 2, "Max retry attempts (not counting first try)");
        manager->createProperty<int>("media.image.retry.baseDelayMs", 500, "Base delay for exponential backoff in ms");

        manager->createProperty<int>("media.image.fast.thumbSize", 256, "Thumbnail size for FAST pipeline");
        manager->createProperty<int>("media.image.balanced.resizeLongEdge", 1024, "Resize long edge for BALANCED pipeline");
        manager->createProperty<int>("media.image.balanced.maxKeypoints", 1000, "Max keypoints to keep for BALANCED pipeline");

        manager->createProperty<std::string>("media.image.quality.onnx.modelPath", "models/clip-RN50.onnx", "ONNX model path for QUALITY pipeline");
        manager->createProperty<int>("media.image.quality.onnx.inputSize", 224, "ONNX input size for QUALITY pipeline");
        manager->createProperty<int>("media.image.quality.embeddingDim", 512, "Embedding dimension for QUALITY pipeline");

        // Create default debug properties
        manager->createProperty<bool>("debug.enabled", false, "Enable debug mode");
        manager->createProperty<bool>("debug.verbose", false, "Enable verbose debug output");

        // Create default file monitoring properties
        manager->createProperty<bool>("file_monitoring.enabled", config.enable_file_monitoring, "Enable file monitoring");
        manager->createProperty<int>("file_monitoring.interval", static_cast<int>(config.file_check_interval.count()), "File check interval in milliseconds");

        // Create default validation properties
        manager->createProperty<bool>("validation.enabled", config.enable_validation, "Enable validation");
        manager->createProperty<bool>("validation.strict", config.strict_validation, "Enable strict validation");

        // Create default duplicate finder properties
        manager->createProperty<bool>("duplicates.finder.enabled", true, "Enable duplicate detection");
        manager->createProperty<int>("duplicates.finder.intervalMs", 3600000, "Duplicate finder interval (1 hour default)");
        manager->createProperty<int>("duplicates.finder.batchSize", 1000, "Files to process per batch");
        manager->createProperty<int>("duplicates.finder.maxGroupSize", 100, "Max duplicates per group");

        // TPM share for duplicate finder
        manager->createProperty<double>("tpm.types.duplicate_finder.share", 1.0, "Thread pool share for duplicate finder");

        // Mode-specific thresholds
        manager->createProperty<double>("duplicates.fast.threshold", 0.90, "pHash similarity threshold (FAST mode)");
        manager->createProperty<double>("duplicates.balanced.threshold", 0.30, "Feature match ratio (BALANCED mode)");

        // QUALITY mode uses range-based thresholds
        manager->createProperty<double>("duplicates.quality.threshold.min", 0.94, "Minimum threshold for QUALITY mode (loosest match)");
        manager->createProperty<double>("duplicates.quality.threshold.max", 0.98, "Maximum threshold for QUALITY mode (strictest match)");
        manager->createProperty<double>("duplicates.quality.minConfidence", 0.90, "Minimum confidence for QUALITY mode (future use)");

        // Representative selection strategy
        manager->createProperty<std::string>("duplicates.representative.strategy", "size_then_age",
                                             "Representative selection: size_then_age | age_then_size");

        // Thumbnail cache configuration
        manager->createProperty<std::string>("cache.thumbnail.location", "cache/thumbnails", "Thumbnail cache directory");
        manager->createProperty<int>("cache.thumbnail.size_limit_mb", 512, "Thumbnail cache size limit in MB");

        // Thumbnail generation configuration
        manager->createProperty<int>("thumbnail.default.size", 256, "Default thumbnail size (128, 256, 512, or 1024)");
        manager->createProperty<std::string>("thumbnail.allowed.sizes", "128,256,512,1024", "Allowed thumbnail sizes");
        manager->createProperty<int>("thumbnail.generation.timeoutMs", 5000, "Thumbnail generation timeout in milliseconds");
        manager->createProperty<int>("thumbnail.jpeg.quality", 85, "JPEG quality for thumbnails (0-100)");

        // TPM share for thumbnail generation
        manager->createProperty<double>("tpm.types.thumbnail_generator.share", 1.0, "Thread pool share for thumbnail generation");

        // Create default media category properties
        // Images
        manager->createProperty<bool>("media.images.jpg", true, "Enable JPEG image processing");
        manager->createProperty<bool>("media.images.jpeg", true, "Enable JPEG image processing");
        manager->createProperty<bool>("media.images.png", true, "Enable PNG image processing");
        manager->createProperty<bool>("media.images.bmp", true, "Enable BMP image processing");
        manager->createProperty<bool>("media.images.gif", true, "Enable GIF image processing");
        manager->createProperty<bool>("media.images.tiff", true, "Enable TIFF image processing");
        manager->createProperty<bool>("media.images.webp", true, "Enable WebP image processing");
        manager->createProperty<bool>("media.images.jp2", true, "Enable JPEG 2000 image processing");
        manager->createProperty<bool>("media.images.ppm", true, "Enable PPM image processing");
        manager->createProperty<bool>("media.images.pgm", true, "Enable PGM image processing");
        manager->createProperty<bool>("media.images.pbm", true, "Enable PBM image processing");
        manager->createProperty<bool>("media.images.pnm", true, "Enable PNM image processing");
        manager->createProperty<bool>("media.images.exr", true, "Enable EXR image processing");
        manager->createProperty<bool>("media.images.hdr", true, "Enable HDR image processing");

        // Video
        manager->createProperty<bool>("media.video.mp4", true, "Enable MP4 video processing");
        manager->createProperty<bool>("media.video.avi", true, "Enable AVI video processing");
        manager->createProperty<bool>("media.video.mov", true, "Enable MOV video processing");
        manager->createProperty<bool>("media.video.mkv", true, "Enable MKV video processing");
        manager->createProperty<bool>("media.video.wmv", true, "Enable WMV video processing");
        manager->createProperty<bool>("media.video.flv", true, "Enable FLV video processing");
        manager->createProperty<bool>("media.video.webm", true, "Enable WebM video processing");
        manager->createProperty<bool>("media.video.m4v", true, "Enable M4V video processing");
        manager->createProperty<bool>("media.video.mpg", true, "Enable MPG video processing");
        manager->createProperty<bool>("media.video.mpeg", true, "Enable MPEG video processing");
        manager->createProperty<bool>("media.video.ts", true, "Enable TS video processing");
        manager->createProperty<bool>("media.video.mts", true, "Enable MTS video processing");
        manager->createProperty<bool>("media.video.m2ts", true, "Enable M2TS video processing");
        manager->createProperty<bool>("media.video.ogv", true, "Enable OGV video processing");

        // Audio
        manager->createProperty<bool>("media.audio.mp3", true, "Enable MP3 audio processing");
        manager->createProperty<bool>("media.audio.wav", true, "Enable WAV audio processing");
        manager->createProperty<bool>("media.audio.flac", true, "Enable FLAC audio processing");
        manager->createProperty<bool>("media.audio.ogg", true, "Enable OGG audio processing");
        manager->createProperty<bool>("media.audio.m4a", true, "Enable M4A audio processing");
        manager->createProperty<bool>("media.audio.aac", true, "Enable AAC audio processing");
        manager->createProperty<bool>("media.audio.opus", true, "Enable Opus audio processing");
        manager->createProperty<bool>("media.audio.wma", true, "Enable WMA audio processing");
        manager->createProperty<bool>("media.audio.aiff", true, "Enable AIFF audio processing");
        manager->createProperty<bool>("media.audio.alac", true, "Enable ALAC audio processing");
        manager->createProperty<bool>("media.audio.amr", true, "Enable AMR audio processing");
        manager->createProperty<bool>("media.audio.au", true, "Enable AU audio processing");

        // Raw Images (subcategory under images)
        manager->createProperty<bool>("media.images.raw.cr2", true, "Enable Canon CR2 raw image processing");
        manager->createProperty<bool>("media.images.raw.nef", true, "Enable Nikon NEF raw image processing");
        manager->createProperty<bool>("media.images.raw.arw", true, "Enable Sony ARW raw image processing");
        manager->createProperty<bool>("media.images.raw.dng", true, "Enable Adobe DNG raw image processing");
        manager->createProperty<bool>("media.images.raw.raf", true, "Enable Fujifilm RAF raw image processing");
        manager->createProperty<bool>("media.images.raw.rw2", true, "Enable Panasonic RW2 raw image processing");
        manager->createProperty<bool>("media.images.raw.orf", true, "Enable Olympus ORF raw image processing");
        manager->createProperty<bool>("media.images.raw.pef", true, "Enable Pentax PEF raw image processing");
        manager->createProperty<bool>("media.images.raw.srw", true, "Enable Samsung SRW raw image processing");
        manager->createProperty<bool>("media.images.raw.kdc", true, "Enable Kodak KDC raw image processing");
        manager->createProperty<bool>("media.images.raw.dcr", true, "Enable Kodak DCR raw image processing");
        manager->createProperty<bool>("media.images.raw.mos", true, "Enable Leaf MOS raw image processing");
        manager->createProperty<bool>("media.images.raw.mrw", true, "Enable Minolta MRW raw image processing");
        manager->createProperty<bool>("media.images.raw.raw", true, "Enable generic RAW image processing");
        manager->createProperty<bool>("media.images.raw.bay", true, "Enable Casio BAY raw image processing");
        manager->createProperty<bool>("media.images.raw.3fr", true, "Enable Hasselblad 3FR raw image processing");
        manager->createProperty<bool>("media.images.raw.fff", true, "Enable Hasselblad FFF raw image processing");
        manager->createProperty<bool>("media.images.raw.mef", true, "Enable Mamiya MEF raw image processing");
        manager->createProperty<bool>("media.images.raw.iiq", true, "Enable Phase One IIQ raw image processing");
        manager->createProperty<bool>("media.images.raw.rwz", true, "Enable Rawzor RWZ raw image processing");
        manager->createProperty<bool>("media.images.raw.nrw", true, "Enable Nikon NRW raw image processing");
        manager->createProperty<bool>("media.images.raw.rwl", true, "Enable Leica RWL raw image processing");
    }

    void ConfigManagerFactory::setupValidationCallbacks(UnifiedObservableConfigManager *manager, const ConfigManagerConfig &config)
    {
        if (!manager)
        {
            return;
        }

        // Add custom validation for server.port
        manager->registerValidationCallback("server.port", [](const std::string &key, const std::any &value) -> bool
                                            {
            try
            {
                int port = std::any_cast<int>(value);
                return port > 0 && port <= 65535;
            }
            catch (const std::bad_any_cast&)
            {
                return false;
            } });

        // Add custom validation for server.max_connections
        manager->registerValidationCallback("server.max_connections", [](const std::string &key, const std::any &value) -> bool
                                            {
            try
            {
                int max_conn = std::any_cast<int>(value);
                return max_conn > 0 && max_conn <= 10000;
            }
            catch (const std::bad_any_cast&)
            {
                return false;
            } });

        // Add custom validation for server.timeout
        manager->registerValidationCallback("server.timeout", [](const std::string &key, const std::any &value) -> bool
                                            {
            try
            {
                double timeout = std::any_cast<double>(value);
                return timeout > 0.0 && timeout <= 3600.0;
            }
            catch (const std::bad_any_cast&)
            {
                return false;
            } });

        // Add custom validation for database.session.poolMin
        manager->registerValidationCallback("database.session.poolMin", [](const std::string &key, const std::any &value) -> bool
                                            {
            try
            {
                int pool_min = std::any_cast<int>(value);
                return pool_min >= 1 && pool_min <= 50;
            }
            catch (const std::bad_any_cast&)
            {
                return false;
            } });

        // Add custom validation for database.session.poolMax
        manager->registerValidationCallback("database.session.poolMax", [manager](const std::string &key, const std::any &value) -> bool
                                            {
            try
            {
                int pool_max = std::any_cast<int>(value);
                if (pool_max < 1 || pool_max > 100)
                {
                    return false;
                }
                
                // Ensure poolMax >= poolMin
                int pool_min = manager->getPropertyValue<int>("database.session.poolMin", 1);
                return pool_max >= pool_min;
            }
            catch (const std::bad_any_cast&)
            {
                return false;
            } });

        // Add custom validation for logging.level (case-insensitive)
        manager->registerValidationCallback("logging.level", [](const std::string &key, const std::any &value) -> bool
                                            {
            try
            {
                std::string level = std::any_cast<std::string>(value);
                // Convert to lowercase for case-insensitive comparison
                std::string level_lower = level;
                for (char &c : level_lower) {
                    c = static_cast<char>(::tolower(c));
                }
                std::vector<std::string> valid_levels = {"trace", "debug", "info", "warn", "error", "information", "warning"};
                return std::find(valid_levels.begin(), valid_levels.end(), level_lower) != valid_levels.end();
            }
            catch (const std::bad_any_cast&)
            {
                return false;
            } });
    }

    void ConfigManagerFactory::setupEventCallbacks(UnifiedObservableConfigManager *manager, const ConfigManagerConfig &config)
    {
        if (!manager)
        {
            return;
        }

        // Set up file change callback
        manager->setFileChangeCallback([config](const std::string &file_path)
                                       {
            if (config.emit_file_change_events)
            {
                std::cout << "[CONFIG] File changed: " << file_path << std::endl;
            } });

        // Set up configuration change callback
        manager->subscribeToConfigChanges([config](const ConfigChangeEvent &event)
                                          {
            if (config.emit_programmatic_events && !event.is_file_update)
            {
                std::cout << "[CONFIG] " << event.toString() << std::endl;
            }
            else if (config.emit_file_change_events && event.is_file_update)
            {
                std::cout << "[CONFIG] " << event.toString() << std::endl;
            } });
    }

} // namespace MediaDedup
