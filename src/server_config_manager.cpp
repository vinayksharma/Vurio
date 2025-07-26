#include "core/server_config_manager.hpp"
#include "logging/logger.hpp"
#include <fstream>
#include <iostream>

ServerConfigManager::ServerConfigManager()
{
    Logger::info("ServerConfigManager constructor called");

    // Try to load configuration from file first
    const std::string config_file = "config.yaml";

    // Check if config file exists
    std::ifstream file_check(config_file);
    if (!file_check.good())
    {
        Logger::info("Configuration file not found, creating default config.yaml");
        initializeDefaultConfig();
        if (saveConfig(config_file))
        {
            Logger::info("Default configuration saved to: " + config_file);
        }
        else
        {
            Logger::error("Failed to save default configuration to: " + config_file);
        }
    }
    else
    {
        file_check.close();
    }

    // Now try to load the configuration (either existing or newly created)
    if (loadConfig(config_file))
    {
        Logger::info("Configuration loaded from file: " + config_file);
    }
    else
    {
        Logger::error("Failed to load configuration from file, using hardcoded defaults");
        initializeDefaultConfig();
    }

    Logger::info("ServerConfigManager initialization completed");
}

ServerConfigManager &ServerConfigManager::getInstance()
{
    static ServerConfigManager instance;
    return instance;
}

void ServerConfigManager::initializeDefaultConfig()
{
    std::lock_guard<std::mutex> lock(config_mutex_);
    config_ = YAML::Load(R"(
        auth_secret: "your-secret-key-here"
        dedup_mode: "BALANCED"
        log_level: "INFO"
        server_port: 8080
        server_host: "localhost"
        scan_interval_seconds: 3600
        processing_interval_seconds: 1800
        threading:
          max_processing_threads: 8
          max_scan_threads: 4
          http_server_threads: "auto"
          database_threads: 2
        video_processing:
          FAST:
            skip_duration_seconds: 2
            frames_per_skip: 2
            skip_count: 5
          BALANCED:
            skip_duration_seconds: 1
            frames_per_skip: 2
            skip_count: 8
          QUALITY:
            skip_duration_seconds: 1
            frames_per_skip: 3
            skip_count: 12
    )");
}

DedupMode ServerConfigManager::getDedupMode() const
{
    std::lock_guard<std::mutex> lock(config_mutex_);
    std::string mode_str = config_["dedup_mode"].as<std::string>();
    return DedupModes::fromString(mode_str);
}

std::string ServerConfigManager::getLogLevel() const
{
    std::lock_guard<std::mutex> lock(config_mutex_);
    return config_["log_level"].as<std::string>();
}

int ServerConfigManager::getServerPort() const
{
    std::lock_guard<std::mutex> lock(config_mutex_);
    return config_["server_port"].as<int>();
}

std::string ServerConfigManager::getServerHost() const
{
    std::lock_guard<std::mutex> lock(config_mutex_);
    return config_["server_host"].as<std::string>();
}

std::string ServerConfigManager::getAuthSecret() const
{
    std::lock_guard<std::mutex> lock(config_mutex_);
    return config_["auth_secret"].as<std::string>();
}

YAML::Node ServerConfigManager::getConfig() const
{
    std::lock_guard<std::mutex> lock(config_mutex_);
    return config_;
}

int ServerConfigManager::getScanIntervalSeconds() const
{
    std::lock_guard<std::mutex> lock(config_mutex_);
    if (config_["scan_interval_seconds"])
        return config_["scan_interval_seconds"].as<int>();
    return 3600;
}

int ServerConfigManager::getProcessingIntervalSeconds() const
{
    std::lock_guard<std::mutex> lock(config_mutex_);
    if (config_["processing_interval_seconds"])
        return config_["processing_interval_seconds"].as<int>();
    return 1800;
}

// Thread configuration getters with improved error handling
int ServerConfigManager::getMaxProcessingThreads() const
{
    std::lock_guard<std::mutex> lock(config_mutex_);
    try
    {
        if (config_["threading"] && config_["threading"]["max_processing_threads"])
        {
            int value = config_["threading"]["max_processing_threads"].as<int>();
            // Validate: must be positive and reasonable
            if (value > 0 && value <= 64)
            {
                return value;
            }
            else
            {
                Logger::warn("Invalid max_processing_threads value: " + std::to_string(value) +
                             ", using default: 8");
            }
        }
    }
    catch (const std::exception &e)
    {
        Logger::warn("Error parsing max_processing_threads: " + std::string(e.what()) +
                     ", using default: 8");
    }
    return 8; // Default fallback
}

int ServerConfigManager::getMaxScanThreads() const
{
    std::lock_guard<std::mutex> lock(config_mutex_);
    try
    {
        if (config_["threading"] && config_["threading"]["max_scan_threads"])
        {
            int value = config_["threading"]["max_scan_threads"].as<int>();
            // Validate: must be positive and reasonable
            if (value > 0 && value <= 32)
            {
                return value;
            }
            else
            {
                Logger::warn("Invalid max_scan_threads value: " + std::to_string(value) +
                             ", using default: 4");
            }
        }
    }
    catch (const std::exception &e)
    {
        Logger::warn("Error parsing max_scan_threads: " + std::string(e.what()) +
                     ", using default: 4");
    }
    return 4; // Default fallback
}

std::string ServerConfigManager::getHttpServerThreads() const
{
    std::lock_guard<std::mutex> lock(config_mutex_);
    try
    {
        if (config_["threading"] && config_["threading"]["http_server_threads"])
        {
            std::string value = config_["threading"]["http_server_threads"].as<std::string>();
            // Validate: must be "auto" or a positive integer
            if (value == "auto")
            {
                return value;
            }
            else
            {
                // Try to parse as integer
                int int_value = std::stoi(value);
                if (int_value > 0 && int_value <= 64)
                {
                    return value;
                }
                else
                {
                    Logger::warn("Invalid http_server_threads value: " + value +
                                 ", using default: auto");
                }
            }
        }
    }
    catch (const std::exception &e)
    {
        Logger::warn("Error parsing http_server_threads: " + std::string(e.what()) +
                     ", using default: auto");
    }
    return "auto"; // Default fallback
}

int ServerConfigManager::getDatabaseThreads() const
{
    std::lock_guard<std::mutex> lock(config_mutex_);
    try
    {
        if (config_["threading"] && config_["threading"]["database_threads"])
        {
            int value = config_["threading"]["database_threads"].as<int>();
            // Validate: must be positive and reasonable
            if (value > 0 && value <= 16)
            {
                return value;
            }
            else
            {
                Logger::warn("Invalid database_threads value: " + std::to_string(value) +
                             ", using default: 2");
            }
        }
    }
    catch (const std::exception &e)
    {
        Logger::warn("Error parsing database_threads: " + std::string(e.what()) +
                     ", using default: 2");
    }
    return 2; // Default fallback
}

int ServerConfigManager::getVideoSkipDurationSeconds(DedupMode mode) const
{
    std::lock_guard<std::mutex> lock(config_mutex_);
    std::string mode_str = DedupModes::getModeName(mode);
    if (config_["video_processing"] && config_["video_processing"][mode_str])
    {
        return config_["video_processing"][mode_str]["skip_duration_seconds"].as<int>(1);
    }
    return 1;
}

int ServerConfigManager::getVideoFramesPerSkip(DedupMode mode) const
{
    std::lock_guard<std::mutex> lock(config_mutex_);
    std::string mode_str = DedupModes::getModeName(mode);
    if (config_["video_processing"] && config_["video_processing"][mode_str])
    {
        return config_["video_processing"][mode_str]["frames_per_skip"].as<int>(1);
    }
    return 1;
}

int ServerConfigManager::getVideoSkipCount(DedupMode mode) const
{
    std::lock_guard<std::mutex> lock(config_mutex_);
    std::string mode_str = DedupModes::getModeName(mode);
    if (config_["video_processing"] && config_["video_processing"][mode_str])
    {
        return config_["video_processing"][mode_str]["skip_count"].as<int>(5);
    }
    return 5;
}

void ServerConfigManager::setDedupMode(DedupMode mode)
{
    std::lock_guard<std::mutex> lock(config_mutex_);
    std::string old_mode = config_["dedup_mode"].as<std::string>();
    std::string new_mode;
    switch (mode)
    {
    case DedupMode::FAST:
        new_mode = "FAST";
        break;
    case DedupMode::BALANCED:
        new_mode = "BALANCED";
        break;
    case DedupMode::QUALITY:
        new_mode = "QUALITY";
        break;
    }
    if (old_mode != new_mode)
    {
        YAML::Node old_value;
        old_value = old_mode;
        YAML::Node new_value;
        new_value = new_mode;
        config_["dedup_mode"] = new_mode;
        ConfigEvent event{
            ConfigEventType::DEDUP_MODE_CHANGED,
            "dedup_mode",
            old_value,
            new_value,
            "Dedup mode changed from " + old_mode + " to " + new_mode};
        publishEvent(event);
        saveConfigInternal("config.yaml", config_);
    }
}

void ServerConfigManager::setLogLevel(const std::string &level)
{
    std::lock_guard<std::mutex> lock(config_mutex_);
    std::string old_level = config_["log_level"].as<std::string>();
    if (old_level != level)
    {
        YAML::Node old_value;
        old_value = old_level;
        YAML::Node new_value;
        new_value = level;
        config_["log_level"] = level;
        ConfigEvent event{
            ConfigEventType::LOG_LEVEL_CHANGED,
            "log_level",
            old_value,
            new_value,
            "Log level changed from " + old_level + " to " + level};
        publishEvent(event);
        saveConfigInternal("config.yaml", config_);
    }
}

void ServerConfigManager::setServerPort(int port)
{
    std::lock_guard<std::mutex> lock(config_mutex_);
    int old_port = config_["server_port"].as<int>();
    if (old_port != port)
    {
        YAML::Node old_value;
        old_value = old_port;
        YAML::Node new_value;
        new_value = port;
        config_["server_port"] = port;
        ConfigEvent event{
            ConfigEventType::SERVER_PORT_CHANGED,
            "server_port",
            old_value,
            new_value,
            "Server port changed from " + std::to_string(old_port) + " to " + std::to_string(port)};
        publishEvent(event);
        saveConfigInternal("config.yaml", config_);
    }
}

void ServerConfigManager::setAuthSecret(const std::string &secret)
{
    std::lock_guard<std::mutex> lock(config_mutex_);
    std::string old_secret = config_["auth_secret"].as<std::string>();
    if (old_secret != secret)
    {
        YAML::Node old_value;
        old_value = old_secret;
        YAML::Node new_value;
        new_value = secret;
        config_["auth_secret"] = secret;
        ConfigEvent event{
            ConfigEventType::AUTH_SECRET_CHANGED,
            "auth_secret",
            old_value,
            new_value,
            "Auth secret changed"};
        publishEvent(event);
        saveConfigInternal("config.yaml", config_);
    }
}

void ServerConfigManager::updateConfig(const YAML::Node &new_config)
{
    std::lock_guard<std::mutex> lock(config_mutex_);
    YAML::Node old_config = config_;
    bool config_changed = false;
    for (auto it = new_config.begin(); it != new_config.end(); ++it)
    {
        if (config_[it->first])
        {
            YAML::Node old_value = config_[it->first];
            YAML::Node new_value = it->second;
            if (YAML::Dump(old_value) != YAML::Dump(new_value))
            {
                config_[it->first] = it->second;
                config_changed = true;
                ConfigEvent event{
                    ConfigEventType::GENERAL_CONFIG_CHANGED,
                    it->first.as<std::string>(),
                    old_value,
                    new_value,
                    "Configuration key '" + it->first.as<std::string>() + "' updated"};
                publishEvent(event);
            }
        }
        else
        {
            config_[it->first] = it->second;
            config_changed = true;
        }
    }
    if (config_changed)
    {
        saveConfigInternal("config.yaml", config_);
    }
}

void ServerConfigManager::subscribe(ConfigObserver *observer)
{
    std::lock_guard<std::mutex> lock(observers_mutex_);
    observers_.push_back(observer);
    Logger::info("Configuration observer subscribed");
}

void ServerConfigManager::unsubscribe(ConfigObserver *observer)
{
    std::lock_guard<std::mutex> lock(observers_mutex_);
    observers_.erase(
        std::remove(observers_.begin(), observers_.end(), observer),
        observers_.end());
    Logger::info("Configuration observer unsubscribed");
}

void ServerConfigManager::publishEvent(const ConfigEvent &event)
{
    std::lock_guard<std::mutex> lock(observers_mutex_);
    Logger::info("Publishing config event: " + event.description);
    for (auto observer : observers_)
    {
        try
        {
            observer->onConfigChanged(event);
        }
        catch (const std::exception &e)
        {
            Logger::error("Error in config observer: " + std::string(e.what()));
        }
    }
}

bool ServerConfigManager::loadConfig(const std::string &file_path)
{
    try
    {
        config_ = YAML::LoadFile(file_path);
        if (validateConfig(config_))
        {
            Logger::info("Configuration loaded from: " + file_path);
            return true;
        }
        else
        {
            Logger::error("Invalid configuration in file: " + file_path);
            return false;
        }
    }
    catch (const std::exception &e)
    {
        Logger::error("Error loading config: " + std::string(e.what()));
        return false;
    }
}

bool ServerConfigManager::saveConfig(const std::string &file_path) const
{
    return saveConfigInternal(file_path, config_);
}

bool ServerConfigManager::saveConfigInternal(const std::string &file_path, const YAML::Node &config) const
{
    try
    {
        std::ofstream file(file_path);
        if (!file.is_open())
        {
            Logger::error("Could not open config file for writing: " + file_path);
            return false;
        }
        file << config;
        Logger::info("Configuration saved to: " + file_path);
        return true;
    }
    catch (const std::exception &e)
    {
        Logger::error("Error saving config: " + std::string(e.what()));
        return false;
    }
}

bool ServerConfigManager::validateConfig(const YAML::Node &config) const
{
    std::vector<std::string> required_fields = {
        "dedup_mode", "log_level", "server_port", "server_host", "auth_secret"};
    for (const auto &field : required_fields)
    {
        if (!config[field])
        {
            Logger::error("Missing required config field: " + field);
            return false;
        }
    }
    int port = config["server_port"].as<int>();
    if (port <= 0 || port > 65535)
    {
        Logger::error("Invalid server port: " + std::to_string(port));
        return false;
    }
    std::string mode = config["dedup_mode"].as<std::string>();
    if (mode != "FAST" && mode != "BALANCED" && mode != "QUALITY")
    {
        Logger::error("Invalid dedup mode: " + mode);
        return false;
    }
    std::string log_level = config["log_level"].as<std::string>();
    if (log_level != "TRACE" && log_level != "DEBUG" && log_level != "INFO" &&
        log_level != "WARN" && log_level != "ERROR")
    {
        Logger::error("Invalid log level: " + log_level);
        return false;
    }
    return true;
}