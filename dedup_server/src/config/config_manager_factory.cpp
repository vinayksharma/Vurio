#include "config/config_manager_factory.hpp"
#include <Poco/Logger.h>
#include <filesystem>
#include <stdexcept>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <nlohmann/json.hpp>

namespace MediaDedup
{

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
        config.enable_events = true;
        config.emit_file_change_events = false;
        config.emit_programmatic_events = true;
        config.strict_validation = false;

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
        config.enable_events = true;
        config.emit_file_change_events = true;
        config.emit_programmatic_events = true;
        config.strict_validation = true;
        config.reload_interval = std::chrono::milliseconds(2000);
        config.file_check_interval = std::chrono::milliseconds(1000);

        return createWithConfig(config);
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
                Poco::Logger::get("Config").warning(desc_path + " not found. Descriptions will be empty.");
            }
        } catch (const std::exception& e) {
            Poco::Logger::get("Config").error("Failed to parse config.descriptions.json: " + std::string(e.what()));
        }

        auto getDesc = [&descriptions](const std::string& key) -> std::string {
            if (descriptions.contains(key) && descriptions[key].is_string()) {
                return descriptions[key].get<std::string>();
            }
            return "No description available";
        };

        auto getLongDesc = [&descriptions](const std::string& key) -> std::string {
            std::string long_key = key + ".long";
            if (descriptions.contains(long_key)) {
                if (descriptions[long_key].is_string()) {
                    return descriptions[long_key].get<std::string>();
                } else {
                    return descriptions[long_key].dump();
                }
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
        // AR-6: the 165-entry property schema lives in
        // config/config.schema.json ({"key": {"type","default"}}); the
        // former ~290-line hand-written createProperty block is this loop.
        // Descriptions still come from config.descriptions.json above.
        // The schema is functional (unlike the cosmetic descriptions): every
        // property the system knows about is registered from it, and
        // setPropertyValue/getPropertyValue silently fall back to defaults
        // for unregistered keys. A manager built without the schema is
        // broken in ways that only surface far downstream — fail hard here.
        nlohmann::json schema;
        std::string schema_path = "config/config.schema.json";
        if (!config.config_file_path.empty()) {
            size_t last_slash = config.config_file_path.find_last_of("/\\");
            if (last_slash != std::string::npos) {
                schema_path = config.config_file_path.substr(0, last_slash) + "/config.schema.json";
            }
        }
        // Probe standard locations when the config-relative path misses.
        if (!std::filesystem::exists(schema_path)) {
            for (const char* candidate : {"config/config.schema.json",
                                          "dedup_server/config/config.schema.json",
                                          "../config/config.schema.json"}) {
                if (std::filesystem::exists(candidate)) { schema_path = candidate; break; }
            }
        }
        std::ifstream sf(schema_path);
        if (!sf.is_open()) {
            throw std::runtime_error(
                "config.schema.json not found (tried '" + schema_path +
                "' and standard locations relative to cwd '" +
                std::filesystem::current_path().string() +
                "'); cannot register config properties.");
        }
        try {
            schema = nlohmann::json::parse(sf);
        } catch (const std::exception& e) {
            throw std::runtime_error("Failed to parse " + schema_path + ": " + e.what());
        }

        for (auto it = schema.begin(); it != schema.end(); ++it) {
            const std::string& key = it.key();
            const nlohmann::json& spec = it.value();
            const std::string type = spec.value("type", "string");
            try {
                if (type == "bool")
                    manager->createProperty<bool>(key, spec.value("default", false), getDesc(key), getLongDesc(key));
                else if (type == "int")
                    manager->createProperty<int>(key, spec.value("default", 0), getDesc(key), getLongDesc(key));
                else if (type == "double")
                    manager->createProperty<double>(key, spec.value("default", 0.0), getDesc(key), getLongDesc(key));
                else
                    manager->createProperty<std::string>(key, spec.value("default", std::string()), getDesc(key), getLongDesc(key));
            } catch (const std::exception& e) {
                throw std::runtime_error("Bad schema entry for property '" + key + "' in " +
                                         schema_path + ": " + e.what());
            }
        }

        // Config-derived properties: their defaults come from the
        // ConfigManagerConfig preset, not the schema.
        manager->createProperty<bool>("file_monitoring.enabled", config.enable_file_monitoring, getDesc("file_monitoring.enabled"), getLongDesc("file_monitoring.enabled"));
        manager->createProperty<int>("file_monitoring.interval", static_cast<int>(config.file_check_interval.count()), getDesc("file_monitoring.interval"), getLongDesc("file_monitoring.interval"));
        manager->createProperty<bool>("validation.enabled", config.enable_validation, getDesc("validation.enabled"), getLongDesc("validation.enabled"));
        manager->createProperty<bool>("validation.strict", config.strict_validation, getDesc("validation.strict"), getLongDesc("validation.strict"));
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

        // Set up file change callback (AR-6: Poco logging, not std::cout —
        // cout bypassed log levels, colors, and capture entirely)
        manager->setFileChangeCallback([config](const std::string &file_path)
                                       {
            if (config.emit_file_change_events)
            {
                Poco::Logger::get("Config").information("File changed: " + file_path);
            } });

        // Set up configuration change callback
        manager->subscribeToConfigChanges([config](const ConfigChangeEvent &event)
                                          {
            if ((config.emit_programmatic_events && !event.is_file_update) ||
                (config.emit_file_change_events && event.is_file_update))
            {
                Poco::Logger::get("Config").information(event.toString());
            } });
    }

} // namespace MediaDedup
