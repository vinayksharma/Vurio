#pragma once

#include <string>
#include <map>
#include <functional>
#include <vector>
#include <memory>
#include <mutex>
#include <yaml-cpp/yaml.h>
#include "core/dedup_modes.hpp"

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
    YAML::Node old_value;
    YAML::Node new_value;
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
    YAML::Node getConfig() const;
    int getScanIntervalSeconds() const;
    int getProcessingIntervalSeconds() const;

    // Thread configuration getters
    int getMaxProcessingThreads() const;
    int getMaxScanThreads() const;
    std::string getHttpServerThreads() const;
    int getDatabaseThreads() const;

    // Processing configuration getters
    int getProcessingBatchSize() const;

    // File type configuration getters
    std::map<std::string, bool> getSupportedFileTypes() const;
    std::map<std::string, bool> getTranscodingFileTypes() const;

    // File type utility methods
    std::vector<std::string> getEnabledFileTypes() const;
    bool needsTranscoding(const std::string &file_extension) const;

    // Category-specific enabled extensions
    std::vector<std::string> getEnabledImageExtensions() const;
    std::vector<std::string> getEnabledVideoExtensions() const;
    std::vector<std::string> getEnabledAudioExtensions() const;

    // Cache configuration getters
    uint32_t getDecoderCacheSizeMB() const;

    // Decoder configuration getters
    int getMaxDecoderThreads() const;

    // Quality stack configuration
    bool getPreProcessQualityStack() const;

    // Video processing configuration accessors
    int getVideoSkipDurationSeconds(DedupMode mode) const;
    int getVideoFramesPerSkip(DedupMode mode) const;
    int getVideoSkipCount(DedupMode mode) const;

    // Configuration setters with event publishing
    void setDedupMode(DedupMode mode);
    void setLogLevel(const std::string &level);
    void setServerPort(int port);
    void setAuthSecret(const std::string &secret);
    void updateConfig(const YAML::Node &new_config);

    // Observer management
    void subscribe(ConfigObserver *observer);
    void unsubscribe(ConfigObserver *observer);

    // Configuration persistence
    bool loadConfig(const std::string &file_path);
    bool saveConfig(const std::string &file_path) const;

    // Configuration validation
    bool validateConfig(const YAML::Node &config) const;

private:
    ServerConfigManager();
    ~ServerConfigManager() = default;
    ServerConfigManager(const ServerConfigManager &) = delete;
    ServerConfigManager &operator=(const ServerConfigManager &) = delete;

    // Internal methods
    void publishEvent(const ConfigEvent &event);
    void initializeDefaultConfig();
    bool saveConfigInternal(const std::string &file_path, const YAML::Node &config) const;

    // Configuration storage
    mutable std::mutex config_mutex_;
    YAML::Node config_;

    // Observers
    mutable std::mutex observers_mutex_;
    std::vector<ConfigObserver *> observers_;
};

// TODO: IMPLEMENTATION NOTES
//
// This configuration manager provides:
// 1. Centralized configuration storage
// 2. Reactive publishing to subscribed services
// 3. Configuration persistence to/from YAML files
// 4. Thread-safe operations
// 5. Event-driven architecture for configuration changes
//
// Services can subscribe to configuration changes and react accordingly:
// - Auth service can react to secret changes
// - Dedup service can react to mode changes
// - Logging service can react to log level changes
// - Server can react to port changes