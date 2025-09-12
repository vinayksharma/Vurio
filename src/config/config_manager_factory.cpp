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
        manager->createProperty<double>("tpm.types.fileScan.share", 1.0, "File scan type share");

        // Create default debug properties
        manager->createProperty<bool>("debug.enabled", false, "Enable debug mode");
        manager->createProperty<bool>("debug.verbose", false, "Enable verbose debug output");

        // Create default file monitoring properties
        manager->createProperty<bool>("file_monitoring.enabled", config.enable_file_monitoring, "Enable file monitoring");
        manager->createProperty<int>("file_monitoring.interval", static_cast<int>(config.file_check_interval.count()), "File check interval in milliseconds");

        // Create default validation properties
        manager->createProperty<bool>("validation.enabled", config.enable_validation, "Enable validation");
        manager->createProperty<bool>("validation.strict", config.strict_validation, "Enable strict validation");
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

        // Add custom validation for logging.level
        manager->registerValidationCallback("logging.level", [](const std::string &key, const std::any &value) -> bool
                                            {
            try
            {
                std::string level = std::any_cast<std::string>(value);
                std::vector<std::string> valid_levels = {"trace", "debug", "info", "warn", "error"};
                return std::find(valid_levels.begin(), valid_levels.end(), level) != valid_levels.end();
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
