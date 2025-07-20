#pragma once

#include <string>
#include <map>
#include <functional>
#include <vector>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include "core/dedup_modes.hpp"

using json = nlohmann::json;

/**
 * @brief Configuration change event types
 */
enum class ConfigEventType
{
    DEDUP_MODE_CHANGED,
    LOG_LEVEL_CHANGED,
    SERVER_PORT_CHANGED,
    AUTH_SECRET_CHANGED,
    GENERAL_CONFIG_CHANGED
};

/**
 * @brief Configuration change event
 */
struct ConfigEvent
{
    ConfigEventType type;
    std::string key;
    json old_value;
    json new_value;
    std::string description;
};

/**
 * @brief Observer interface for configuration changes
 */
class ConfigObserver
{
public:
    virtual ~ConfigObserver() = default;
    virtual void onConfigChanged(const ConfigEvent &event) = 0;
};

/**
 * @brief Server configuration manager with reactive publishing
 */
class ServerConfigManager
{
public:
    // Singleton pattern
    static ServerConfigManager &getInstance();

    // Configuration getters
    DedupMode getDedupMode() const;
    std::string getLogLevel() const;
    int getServerPort() const;
    std::string getServerHost() const;
    std::string getAuthSecret() const;
    json getConfig() const;
    json getScanSchedules() const;
    int getDefaultScanInterval() const;

    // Configuration setters with event publishing
    void setDedupMode(DedupMode mode);
    void setLogLevel(const std::string &level);
    void setServerPort(int port);
    void setAuthSecret(const std::string &secret);
    void updateConfig(const json &new_config);

    // Observer management
    void subscribe(ConfigObserver *observer);
    void unsubscribe(ConfigObserver *observer);

    // Configuration persistence
    bool loadConfig(const std::string &file_path);
    bool saveConfig(const std::string &file_path) const;

    // Configuration validation
    bool validateConfig(const json &config) const;

private:
    ServerConfigManager();
    ~ServerConfigManager() = default;
    ServerConfigManager(const ServerConfigManager &) = delete;
    ServerConfigManager &operator=(const ServerConfigManager &) = delete;

    // Internal methods
    void publishEvent(const ConfigEvent &event);
    void initializeDefaultConfig();
    bool saveConfigInternal(const std::string &file_path, const json &config) const;

    // Configuration storage
    mutable std::mutex config_mutex_;
    json config_;

    // Observers
    mutable std::mutex observers_mutex_;
    std::vector<ConfigObserver *> observers_;
};

// TODO: IMPLEMENTATION NOTES
//
// This configuration manager provides:
// 1. Centralized configuration storage
// 2. Reactive publishing to subscribed services
// 3. Configuration persistence to/from JSON files
// 4. Thread-safe operations
// 5. Event-driven architecture for configuration changes
//
// Services can subscribe to configuration changes and react accordingly:
// - Auth service can react to secret changes
// - Dedup service can react to mode changes
// - Logging service can react to log level changes
// - Server can react to port changes