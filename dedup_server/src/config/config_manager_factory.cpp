#include "config/config_manager_factory.hpp"
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <nlohmann/json.hpp>

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

        nlohmann::json descriptions;
        try {
            // Assume config.descriptions.json is located in the same directory as config.yaml, or in a standard location.
            // If config_file_path is empty, default to "config/config.descriptions.json"
            std::string desc_path = "config/config.descriptions.json";
            if (!config.config_file_path.empty()) {
                size_t last_slash = config.config_file_path.find_last_of("/\\");
                if (last_slash != std::string::npos) {
                    desc_path = config.config_file_path.substr(0, last_slash) + "/config.descriptions.json";
                }
            }
            
            std::ifstream f(desc_path);
            if (f.is_open()) {
                descriptions = nlohmann::json::parse(f);
            } else {
                std::cerr << "[WARNING] " << desc_path << " not found. Descriptions will be empty." << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "[ERROR] Failed to parse config.descriptions.json: " << e.what() << std::endl;
        }

        auto getDesc = [&descriptions](const std::string& key) -> std::string {
            if (descriptions.contains(key) && descriptions[key].is_string()) {
                return descriptions[key].get<std::string>();
            }
            return "No description available";
        };

        auto getLongDesc = [&descriptions](const std::string& key) -> std::string {
            std::string long_key = key + ".long";
            if (descriptions.contains(long_key) && descriptions[long_key].is_string()) {
                return descriptions[long_key].get<std::string>();
            }
            return "Detailed information for " + key;
        };

        auto getOptions = [&descriptions](const std::string& key) -> std::string {
            std::string options_key = key + ".options";
            if (descriptions.contains(options_key) && descriptions[options_key].is_string()) {
                return descriptions[options_key].get<std::string>();
            }
            return "";
        };

        // Create default server properties (matching CONFIGURATION_REFERENCE.md)
        manager->createProperty<std::string>("server.host", "0.0.0.0", getDesc("server.host"), getLongDesc("server.host"));
        manager->createProperty<int>("server.port", 8080, getDesc("server.port"), getLongDesc("server.port"));
        manager->createProperty<std::string>("server.name", "Media Deduplication Server", getDesc("server.name"), getLongDesc("server.name"));
        manager->createProperty<std::string>("server.processName", "media_dedup_server", getDesc("server.processName"), getLongDesc("server.processName"));
        manager->createProperty<bool>("server.instanceCheck.enabled", true, getDesc("server.instanceCheck.enabled"), getLongDesc("server.instanceCheck.enabled"));
        manager->createProperty<int>("server.instanceCheck.bufferSize", 128, getDesc("server.instanceCheck.bufferSize"), getLongDesc("server.instanceCheck.bufferSize"));
        manager->createProperty<int>("server.max_connections", 100, getDesc("server.max_connections"), getLongDesc("server.max_connections"));
        manager->createProperty<double>("server.timeout", 30.0, getDesc("server.timeout"), getLongDesc("server.timeout"));
        manager->createProperty<std::string>("server.http.threadPool.maxThreads", "auto", getDesc("server.http.threadPool.maxThreads"), getLongDesc("server.http.threadPool.maxThreads"));
        manager->createProperty<int>("server.http.threadPool.maxQueued", 50, getDesc("server.http.threadPool.maxQueued"), getLongDesc("server.http.threadPool.maxQueued"));

        // Create default database properties
        manager->createProperty<std::string>("database.path", "data/dedup_server.db", getDesc("database.path"), getLongDesc("database.path"));
        manager->createProperty<int>("database.session.acquireTimeoutMs", 3000, getDesc("database.session.acquireTimeoutMs"), getLongDesc("database.session.acquireTimeoutMs"));
        manager->createProperty<int>("database.session.acquireBackoffMs", 50, getDesc("database.session.acquireBackoffMs"), getLongDesc("database.session.acquireBackoffMs"));
        manager->createProperty<int>("database.session.poolMin", 4, getDesc("database.session.poolMin"), getLongDesc("database.session.poolMin"));
        manager->createProperty<int>("database.session.poolMax", 20, getDesc("database.session.poolMax"), getLongDesc("database.session.poolMax"));
        manager->createProperty<bool>("database.vector.enabled", false, getDesc("database.vector.enabled"), getLongDesc("database.vector.enabled"));
        manager->createProperty<std::string>("database.vector.extensionPath", "", getDesc("database.vector.extensionPath"), getLongDesc("database.vector.extensionPath"));
        manager->createProperty<int>("database.sqlite.cacheSize", 1000, getDesc("database.sqlite.cacheSize"), getLongDesc("database.sqlite.cacheSize"));
        manager->createProperty<std::string>("database.sqlite.tempStore", "memory", getDesc("database.sqlite.tempStore"), getLongDesc("database.sqlite.tempStore"), getOptions("database.sqlite.tempStore"));

        // Create default logging properties
        manager->createProperty<std::string>("logging.level", config.log_level, getDesc("logging.level"), getLongDesc("logging.level"), getOptions("logging.level"));
        manager->createProperty<bool>("logging.enable_console", true, getDesc("logging.enable_console"), getLongDesc("logging.enable_console"));
        manager->createProperty<bool>("logging.enable_file", false, getDesc("logging.enable_file"), getLongDesc("logging.enable_file"));

        // Create default files manager properties
        manager->createProperty<bool>("files.manager.enabled", true, getDesc("files.manager.enabled"), getLongDesc("files.manager.enabled"));
        manager->createProperty<int>("files.manager.scan.intervalMs", 500, getDesc("files.manager.scan.intervalMs"), getLongDesc("files.manager.scan.intervalMs"));
        manager->createProperty<bool>("files.manager.useInMemoryIndex", false, getDesc("files.manager.useInMemoryIndex"), getLongDesc("files.manager.useInMemoryIndex"));
        manager->createProperty<bool>("files.service.immediateJobTrigger.enabled", true, getDesc("files.service.immediateJobTrigger.enabled"), getLongDesc("files.service.immediateJobTrigger.enabled"));
        manager->createProperty<int>("files.service.immediateJobTrigger.timeoutMs", 30000, getDesc("files.service.immediateJobTrigger.timeoutMs"), getLongDesc("files.service.immediateJobTrigger.timeoutMs"));

        // Create default scheduler properties
        manager->createProperty<bool>("scheduler.jitter.enabled", false, getDesc("scheduler.jitter.enabled"), getLongDesc("scheduler.jitter.enabled"));
        manager->createProperty<int>("scheduler.jitter.percent", 0, getDesc("scheduler.jitter.percent"), getLongDesc("scheduler.jitter.percent"));
        manager->createProperty<std::string>("scheduler.drift.mode", "anchored", getDesc("scheduler.drift.mode"), getLongDesc("scheduler.drift.mode"), getOptions("scheduler.drift.mode"));
        manager->createProperty<int>("scheduler.drift.maxDriftMs", 60000, getDesc("scheduler.drift.maxDriftMs"), getLongDesc("scheduler.drift.maxDriftMs"));
        manager->createProperty<bool>("scheduler.backoff.enabled", true, getDesc("scheduler.backoff.enabled"), getLongDesc("scheduler.backoff.enabled"));
        manager->createProperty<int>("scheduler.backoff.initialMs", 1000, getDesc("scheduler.backoff.initialMs"), getLongDesc("scheduler.backoff.initialMs"));
        manager->createProperty<double>("scheduler.backoff.multiplier", 2.0, getDesc("scheduler.backoff.multiplier"), getLongDesc("scheduler.backoff.multiplier"));
        manager->createProperty<int>("scheduler.backoff.maxMs", 30000, getDesc("scheduler.backoff.maxMs"), getLongDesc("scheduler.backoff.maxMs"));
        manager->createProperty<int>("scheduler.backoff.jitterPercent", 10, getDesc("scheduler.backoff.jitterPercent"), getLongDesc("scheduler.backoff.jitterPercent"));
        manager->createProperty<std::string>("scheduler.jobs", "", getDesc("scheduler.jobs"), getLongDesc("scheduler.jobs"));
        manager->createProperty<int>("scheduler.maxConsecutiveFailures", 5, getDesc("scheduler.maxConsecutiveFailures"), getLongDesc("scheduler.maxConsecutiveFailures"));

        // Create default TPM properties
        manager->createProperty<std::string>("tpm.pool.max", "auto", getDesc("tpm.pool.max"), getLongDesc("tpm.pool.max"));
        manager->createProperty<int>("tpm.killTimeoutMs", 10000, getDesc("tpm.killTimeoutMs"), getLongDesc("tpm.killTimeoutMs"));
        manager->createProperty<int>("tpm.thread.idleTimeoutSeconds", 120, getDesc("tpm.thread.idleTimeoutSeconds"), getLongDesc("tpm.thread.idleTimeoutSeconds"));
        manager->createProperty<int>("tpm.thread.stackSizeKB", 1024, getDesc("tpm.thread.stackSizeKB"), getLongDesc("tpm.thread.stackSizeKB"));
        manager->createProperty<double>("tpm.types.fileScan.share", 1.0, getDesc("tpm.types.fileScan.share"), getLongDesc("tpm.types.fileScan.share"));

        // Create default media processor properties
        manager->createProperty<bool>("media.processor.enabled", true, getDesc("media.processor.enabled"), getLongDesc("media.processor.enabled"));
        manager->createProperty<int>("media.processor.intervalMs", 30000, getDesc("media.processor.intervalMs"), getLongDesc("media.processor.intervalMs"));
        manager->createProperty<int>("media.processor.maxQueueSize", 1000, getDesc("media.processor.maxQueueSize"), getLongDesc("media.processor.maxQueueSize"));

        // TPM thread type shares for media processing (consolidated naming: tpm.types.<type>.share)
        manager->createProperty<double>("tpm.types.media_processor.share", 1.0, getDesc("tpm.types.media_processor.share"), getLongDesc("tpm.types.media_processor.share"));
        manager->createProperty<double>("tpm.types.image_processor.share", 1.0, getDesc("tpm.types.image_processor.share"), getLongDesc("tpm.types.image_processor.share"));
        manager->createProperty<double>("tpm.types.audio_processor.share", 1.0, getDesc("tpm.types.audio_processor.share"), getLongDesc("tpm.types.audio_processor.share"));
        manager->createProperty<double>("tpm.types.video_processor.share", 1.0, getDesc("tpm.types.video_processor.share"), getLongDesc("tpm.types.video_processor.share"));

        // Image pipelines SAFE TREO configuration
        manager->createProperty<int>("media.image.timeoutMs", 30000, getDesc("media.image.timeoutMs"), getLongDesc("media.image.timeoutMs"));
        manager->createProperty<bool>("media.image.retry.enabled", true, getDesc("media.image.retry.enabled"), getLongDesc("media.image.retry.enabled"));
        manager->createProperty<int>("media.image.retry.maxAttempts", 2, getDesc("media.image.retry.maxAttempts"), getLongDesc("media.image.retry.maxAttempts"));
        manager->createProperty<int>("media.image.retry.baseDelayMs", 500, getDesc("media.image.retry.baseDelayMs"), getLongDesc("media.image.retry.baseDelayMs"));

        manager->createProperty<std::string>("media.image.onnx.modelPath", "models/clip-image-vitb32.onnx", getDesc("media.image.onnx.modelPath"), getLongDesc("media.image.onnx.modelPath"));
        manager->createProperty<int>("media.image.onnx.inputSize", 224, getDesc("media.image.onnx.inputSize"), getLongDesc("media.image.onnx.inputSize"));
        manager->createProperty<bool>("media.image.onnx.sessionCache.enabled", true, getDesc("media.image.onnx.sessionCache.enabled"), getLongDesc("media.image.onnx.sessionCache.enabled"));
        manager->createProperty<int>("media.image.onnx.sessionCache.maxSessions", 4, getDesc("media.image.onnx.sessionCache.maxSessions"), getLongDesc("media.image.onnx.sessionCache.maxSessions"));
        manager->createProperty<int>("media.image.embeddingDim", 512, getDesc("media.image.embeddingDim"), getLongDesc("media.image.embeddingDim"));
        manager->createProperty<bool>("media.image.transcoding.enabled", true, getDesc("media.image.transcoding.enabled"), getLongDesc("media.image.transcoding.enabled"));
        manager->createProperty<int>("media.image.transcoding.timeoutMs", 120000, getDesc("media.image.transcoding.timeoutMs"), getLongDesc("media.image.transcoding.timeoutMs"));
        manager->createProperty<bool>("media.image.transcoding.preserveMetadata", true, getDesc("media.image.transcoding.preserveMetadata"), getLongDesc("media.image.transcoding.preserveMetadata"));

        // Create default debug properties
        manager->createProperty<bool>("debug.enabled", false, getDesc("debug.enabled"), getLongDesc("debug.enabled"));
        manager->createProperty<bool>("debug.verbose", false, getDesc("debug.verbose"), getLongDesc("debug.verbose"));

        // Create default file monitoring properties
        manager->createProperty<bool>("file_monitoring.enabled", config.enable_file_monitoring, getDesc("file_monitoring.enabled"), getLongDesc("file_monitoring.enabled"));
        manager->createProperty<int>("file_monitoring.interval", static_cast<int>(config.file_check_interval.count()), getDesc("file_monitoring.interval"), getLongDesc("file_monitoring.interval"));

        // Create default validation properties
        manager->createProperty<bool>("validation.enabled", config.enable_validation, getDesc("validation.enabled"), getLongDesc("validation.enabled"));
        manager->createProperty<bool>("validation.strict", config.strict_validation, getDesc("validation.strict"), getLongDesc("validation.strict"));

        // Create default duplicate finder properties
        manager->createProperty<bool>("duplicates.finder.enabled", true, getDesc("duplicates.finder.enabled"), getLongDesc("duplicates.finder.enabled"));
        manager->createProperty<int>("duplicates.finder.intervalMs", 3600000, getDesc("duplicates.finder.intervalMs"), getLongDesc("duplicates.finder.intervalMs"));
        manager->createProperty<int>("duplicates.finder.batchSize", 1000, getDesc("duplicates.finder.batchSize"), getLongDesc("duplicates.finder.batchSize"));
        manager->createProperty<int>("duplicates.finder.maxGroupSize", 100, getDesc("duplicates.finder.maxGroupSize"), getLongDesc("duplicates.finder.maxGroupSize"));
        manager->createProperty<int>("duplicates.finder.knnK", 200, getDesc("duplicates.finder.knnK"), getLongDesc("duplicates.finder.knnK"));
        manager->createProperty<std::string>("duplicates.representative.strategy", "size_then_age", getDesc("duplicates.representative.strategy"), getLongDesc("duplicates.representative.strategy"));
        manager->createProperty<bool>("duplicates.metadata.filtering.enabled", false, getDesc("duplicates.metadata.filtering.enabled"), getLongDesc("duplicates.metadata.filtering.enabled"));
        manager->createProperty<double>("duplicates.metadata.fileSizeTolerance", 0.20000000000000001, getDesc("duplicates.metadata.fileSizeTolerance"), getLongDesc("duplicates.metadata.fileSizeTolerance"));
        manager->createProperty<double>("duplicates.metadata.dimensionTolerance", 0.10000000000000001, getDesc("duplicates.metadata.dimensionTolerance"), getLongDesc("duplicates.metadata.dimensionTolerance"));
        manager->createProperty<double>("duplicates.metadata.aspectRatioTolerance", 0.050000000000000003, getDesc("duplicates.metadata.aspectRatioTolerance"), getLongDesc("duplicates.metadata.aspectRatioTolerance"));
        manager->createProperty<bool>("duplicates.metadata.requireSameFormat", false, getDesc("duplicates.metadata.requireSameFormat"), getLongDesc("duplicates.metadata.requireSameFormat"));

        // TPM share for duplicate finder
        manager->createProperty<double>("tpm.types.duplicate_finder.share", 1.0, getDesc("tpm.types.duplicate_finder.share"), getLongDesc("tpm.types.duplicate_finder.share"));

        // Similarity threshold range (two-tier: similarity vs duplicate)
        manager->createProperty<double>("duplicates.threshold.similarity", 0.92, getDesc("duplicates.threshold.similarity"), getLongDesc("duplicates.threshold.similarity"));
        manager->createProperty<double>("duplicates.threshold.duplicate", 0.96, getDesc("duplicates.threshold.duplicate"), getLongDesc("duplicates.threshold.duplicate"));

        // Deprecated: kept for backward compatibility, map to similarity/duplicate

        // Purity order criteria
        manager->createProperty<std::string>("duplicates.purity.order", "CAMERA_RAW, EXIF_CAPTURE_DATE, FILE_SIZE", getDesc("duplicates.purity.order"), getLongDesc("duplicates.purity.order"));

        manager->createProperty<std::string>("cache.thumbnail.location", "./thumbnails_cache", getDesc("cache.thumbnail.location"), getLongDesc("cache.thumbnail.location"));
        manager->createProperty<std::string>("cache.disk.location", "./previews", getDesc("cache.disk.location"), getLongDesc("cache.disk.location"));
        manager->createProperty<int>("cache.thumbnail.size_limit_mb", 512, getDesc("cache.thumbnail.size_limit_mb"), getLongDesc("cache.thumbnail.size_limit_mb"));
        manager->createProperty<int>("cache.disk.size_limit_mb", 2048, getDesc("cache.disk.size_limit_mb"), getLongDesc("cache.disk.size_limit_mb"));
        manager->createProperty<bool>("cache.thumbnail.clearOnStartup", false, getDesc("cache.thumbnail.clearOnStartup"), getLongDesc("cache.thumbnail.clearOnStartup"));

        // Thumbnail generation configuration
        manager->createProperty<int>("thumbnail.default.size", 256, getDesc("thumbnail.default.size"), getLongDesc("thumbnail.default.size"));
        manager->createProperty<std::string>("thumbnail.allowed.sizes", "128,256,512,1024", getDesc("thumbnail.allowed.sizes"), getLongDesc("thumbnail.allowed.sizes"));
        manager->createProperty<int>("thumbnail.generation.timeoutMs", 5000, getDesc("thumbnail.generation.timeoutMs"), getLongDesc("thumbnail.generation.timeoutMs"));
        manager->createProperty<int>("thumbnail.jpeg.quality", 85, getDesc("thumbnail.jpeg.quality"), getLongDesc("thumbnail.jpeg.quality"));
        manager->createProperty<double>("thumbnail.qhd_preview.threshold_mb", 5.0, getDesc("thumbnail.qhd_preview.threshold_mb"), getLongDesc("thumbnail.qhd_preview.threshold_mb"));
        manager->createProperty<bool>("thumbnail.pipeline.enabled", true, getDesc("thumbnail.pipeline.enabled"), getLongDesc("thumbnail.pipeline.enabled"));
        manager->createProperty<bool>("thumbnail.pipeline.onlyIfMissing", true, getDesc("thumbnail.pipeline.onlyIfMissing"), getLongDesc("thumbnail.pipeline.onlyIfMissing"));
        manager->createProperty<int>("thumbnail.pipeline.size", 256, getDesc("thumbnail.pipeline.size"), getLongDesc("thumbnail.pipeline.size"));

        // TPM share for thumbnail generation
        manager->createProperty<double>("tpm.types.thumbnail_generator.share", 1.0, getDesc("tpm.types.thumbnail_generator.share"), getLongDesc("tpm.types.thumbnail_generator.share"));


        // Create default filter properties
        manager->createProperty<bool>("media.filter.enabled", true, getDesc("media.filter.enabled"), getLongDesc("media.filter.enabled"));
        manager->createProperty<int>("media.filter.min_file_size_kb", 10, getDesc("media.filter.min_file_size_kb"), getLongDesc("media.filter.min_file_size_kb"));
        manager->createProperty<int>("media.filter.low_res.min_width", 600, getDesc("media.filter.low_res.min_width"), getLongDesc("media.filter.low_res.min_width"));
        manager->createProperty<int>("media.filter.low_res.min_height", 600, getDesc("media.filter.low_res.min_height"), getLongDesc("media.filter.low_res.min_height"));
        manager->createProperty<std::string>("media.filter.low_res.apply_after_date", "2005-01-01", getDesc("media.filter.low_res.apply_after_date"), getLongDesc("media.filter.low_res.apply_after_date"));


        // Create default media category properties
        // Images
        manager->createProperty<bool>("media.images.jpg", true, getDesc("media.images.jpg"), getLongDesc("media.images.jpg"));
        manager->createProperty<bool>("media.images.jpeg", true, getDesc("media.images.jpeg"), getLongDesc("media.images.jpeg"));
        manager->createProperty<bool>("media.images.png", true, getDesc("media.images.png"), getLongDesc("media.images.png"));
        manager->createProperty<bool>("media.images.bmp", true, getDesc("media.images.bmp"), getLongDesc("media.images.bmp"));
        manager->createProperty<bool>("media.images.gif", true, getDesc("media.images.gif"), getLongDesc("media.images.gif"));
        manager->createProperty<bool>("media.images.tiff", true, getDesc("media.images.tiff"), getLongDesc("media.images.tiff"));
        manager->createProperty<bool>("media.images.tif", true, getDesc("media.images.tif"), getLongDesc("media.images.tif"));
        manager->createProperty<bool>("media.images.webp", true, getDesc("media.images.webp"), getLongDesc("media.images.webp"));
        manager->createProperty<bool>("media.images.jp2", true, getDesc("media.images.jp2"), getLongDesc("media.images.jp2"));
        manager->createProperty<bool>("media.images.ppm", true, getDesc("media.images.ppm"), getLongDesc("media.images.ppm"));
        manager->createProperty<bool>("media.images.pgm", true, getDesc("media.images.pgm"), getLongDesc("media.images.pgm"));
        manager->createProperty<bool>("media.images.pbm", true, getDesc("media.images.pbm"), getLongDesc("media.images.pbm"));
        manager->createProperty<bool>("media.images.pnm", true, getDesc("media.images.pnm"), getLongDesc("media.images.pnm"));
        manager->createProperty<bool>("media.images.exr", true, getDesc("media.images.exr"), getLongDesc("media.images.exr"));
        manager->createProperty<bool>("media.images.hdr", true, getDesc("media.images.hdr"), getLongDesc("media.images.hdr"));

        // Transcode to JPEG before embedding (for formats OpenCV may not read reliably)
        manager->createProperty<bool>("media.images.bmp.transcode", true, getDesc("media.images.bmp.transcode"), getLongDesc("media.images.bmp.transcode"));
        manager->createProperty<bool>("media.images.dib.transcode", true, getDesc("media.images.dib.transcode"), getLongDesc("media.images.dib.transcode"));
        manager->createProperty<bool>("media.images.tif.transcode", true, getDesc("media.images.tif.transcode"), getLongDesc("media.images.tif.transcode"));
        manager->createProperty<bool>("media.images.tiff.transcode", true, getDesc("media.images.tiff.transcode"), getLongDesc("media.images.tiff.transcode"));
        manager->createProperty<bool>("media.images.gif.transcode", true, getDesc("media.images.gif.transcode"), getLongDesc("media.images.gif.transcode"));
        manager->createProperty<bool>("media.images.jp2.transcode", true, getDesc("media.images.jp2.transcode"), getLongDesc("media.images.jp2.transcode"));
        manager->createProperty<bool>("media.images.exr.transcode", true, getDesc("media.images.exr.transcode"), getLongDesc("media.images.exr.transcode"));
        manager->createProperty<bool>("media.images.hdr.transcode", true, getDesc("media.images.hdr.transcode"), getLongDesc("media.images.hdr.transcode"));

        // Video
        manager->createProperty<bool>("media.video.mp4", true, getDesc("media.video.mp4"), getLongDesc("media.video.mp4"));
        manager->createProperty<bool>("media.video.avi", true, getDesc("media.video.avi"), getLongDesc("media.video.avi"));
        manager->createProperty<bool>("media.video.mov", true, getDesc("media.video.mov"), getLongDesc("media.video.mov"));
        manager->createProperty<bool>("media.video.mkv", true, getDesc("media.video.mkv"), getLongDesc("media.video.mkv"));
        manager->createProperty<bool>("media.video.wmv", true, getDesc("media.video.wmv"), getLongDesc("media.video.wmv"));
        manager->createProperty<bool>("media.video.flv", true, getDesc("media.video.flv"), getLongDesc("media.video.flv"));
        manager->createProperty<bool>("media.video.webm", true, getDesc("media.video.webm"), getLongDesc("media.video.webm"));
        manager->createProperty<bool>("media.video.m4v", true, getDesc("media.video.m4v"), getLongDesc("media.video.m4v"));
        manager->createProperty<bool>("media.video.mpg", true, getDesc("media.video.mpg"), getLongDesc("media.video.mpg"));
        manager->createProperty<bool>("media.video.mpeg", true, getDesc("media.video.mpeg"), getLongDesc("media.video.mpeg"));
        manager->createProperty<bool>("media.video.ts", true, getDesc("media.video.ts"), getLongDesc("media.video.ts"));
        manager->createProperty<bool>("media.video.mts", true, getDesc("media.video.mts"), getLongDesc("media.video.mts"));
        manager->createProperty<bool>("media.video.m2ts", true, getDesc("media.video.m2ts"), getLongDesc("media.video.m2ts"));
        manager->createProperty<bool>("media.video.ogv", true, getDesc("media.video.ogv"), getLongDesc("media.video.ogv"));

        // Audio
        manager->createProperty<bool>("media.audio.mp3", true, getDesc("media.audio.mp3"), getLongDesc("media.audio.mp3"));
        manager->createProperty<bool>("media.audio.wav", true, getDesc("media.audio.wav"), getLongDesc("media.audio.wav"));
        manager->createProperty<bool>("media.audio.flac", true, getDesc("media.audio.flac"), getLongDesc("media.audio.flac"));
        manager->createProperty<bool>("media.audio.ogg", true, getDesc("media.audio.ogg"), getLongDesc("media.audio.ogg"));
        manager->createProperty<bool>("media.audio.m4a", true, getDesc("media.audio.m4a"), getLongDesc("media.audio.m4a"));
        manager->createProperty<bool>("media.audio.aac", true, getDesc("media.audio.aac"), getLongDesc("media.audio.aac"));
        manager->createProperty<bool>("media.audio.opus", true, getDesc("media.audio.opus"), getLongDesc("media.audio.opus"));
        manager->createProperty<bool>("media.audio.wma", true, getDesc("media.audio.wma"), getLongDesc("media.audio.wma"));
        manager->createProperty<bool>("media.audio.aiff", true, getDesc("media.audio.aiff"), getLongDesc("media.audio.aiff"));
        manager->createProperty<bool>("media.audio.alac", true, getDesc("media.audio.alac"), getLongDesc("media.audio.alac"));
        manager->createProperty<bool>("media.audio.amr", true, getDesc("media.audio.amr"), getLongDesc("media.audio.amr"));
        manager->createProperty<bool>("media.audio.au", true, getDesc("media.audio.au"), getLongDesc("media.audio.au"));

        // Raw Images (subcategory under images)
        manager->createProperty<bool>("media.images.raw.cr2", true, getDesc("media.images.raw.cr2"), getLongDesc("media.images.raw.cr2"));
        manager->createProperty<bool>("media.images.raw.nef", true, getDesc("media.images.raw.nef"), getLongDesc("media.images.raw.nef"));
        manager->createProperty<bool>("media.images.raw.arw", true, getDesc("media.images.raw.arw"), getLongDesc("media.images.raw.arw"));
        manager->createProperty<bool>("media.images.raw.dng", true, getDesc("media.images.raw.dng"), getLongDesc("media.images.raw.dng"));
        manager->createProperty<bool>("media.images.raw.raf", true, getDesc("media.images.raw.raf"), getLongDesc("media.images.raw.raf"));
        manager->createProperty<bool>("media.images.raw.rw2", true, getDesc("media.images.raw.rw2"), getLongDesc("media.images.raw.rw2"));
        manager->createProperty<bool>("media.images.raw.orf", true, getDesc("media.images.raw.orf"), getLongDesc("media.images.raw.orf"));
        manager->createProperty<bool>("media.images.raw.pef", true, getDesc("media.images.raw.pef"), getLongDesc("media.images.raw.pef"));
        manager->createProperty<bool>("media.images.raw.srw", true, getDesc("media.images.raw.srw"), getLongDesc("media.images.raw.srw"));
        manager->createProperty<bool>("media.images.raw.kdc", true, getDesc("media.images.raw.kdc"), getLongDesc("media.images.raw.kdc"));
        manager->createProperty<bool>("media.images.raw.dcr", true, getDesc("media.images.raw.dcr"), getLongDesc("media.images.raw.dcr"));
        manager->createProperty<bool>("media.images.raw.mos", true, getDesc("media.images.raw.mos"), getLongDesc("media.images.raw.mos"));
        manager->createProperty<bool>("media.images.raw.mrw", true, getDesc("media.images.raw.mrw"), getLongDesc("media.images.raw.mrw"));
        manager->createProperty<bool>("media.images.raw.raw", true, getDesc("media.images.raw.raw"), getLongDesc("media.images.raw.raw"));
        manager->createProperty<bool>("media.images.raw.bay", true, getDesc("media.images.raw.bay"), getLongDesc("media.images.raw.bay"));
        manager->createProperty<bool>("media.images.raw.3fr", true, getDesc("media.images.raw.3fr"), getLongDesc("media.images.raw.3fr"));
        manager->createProperty<bool>("media.images.raw.fff", true, getDesc("media.images.raw.fff"), getLongDesc("media.images.raw.fff"));
        manager->createProperty<bool>("media.images.raw.mef", true, getDesc("media.images.raw.mef"), getLongDesc("media.images.raw.mef"));
        manager->createProperty<bool>("media.images.raw.iiq", true, getDesc("media.images.raw.iiq"), getLongDesc("media.images.raw.iiq"));
        manager->createProperty<bool>("media.images.raw.rwz", true, getDesc("media.images.raw.rwz"), getLongDesc("media.images.raw.rwz"));
        manager->createProperty<bool>("media.images.raw.nrw", true, getDesc("media.images.raw.nrw"), getLongDesc("media.images.raw.nrw"));
        manager->createProperty<bool>("media.images.raw.rwl", true, getDesc("media.images.raw.rwl"), getLongDesc("media.images.raw.rwl"));
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
