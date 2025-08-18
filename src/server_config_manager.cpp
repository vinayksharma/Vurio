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
    // Initialize last write time for watcher
    try
    {
        last_write_time_ = std::filesystem::last_write_time("config.yaml");
    }
    catch (...)
    {
        // ignore
    }
}

ServerConfigManager &ServerConfigManager::getInstance()
{
    static ServerConfigManager instance;
    return instance;
}

ServerConfigManager::~ServerConfigManager()
{
    stopWatching();
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
        pre_process_quality_stack: false
        threading:
          max_processing_threads: 8
          max_scan_threads: 4
          http_server_threads: "auto"
          database_threads: 2
          max_decoder_threads: 4
        cache:
          decoder_cache_size_mb: 1024
        processing:
          batch_size: 100
        cache_cleanup:
          fully_processed_age_days: 7
          partially_processed_age_days: 3
          unprocessed_age_days: 1
          require_all_modes: true
          cleanup_threshold_percent: 80
        categories:
          images:
            jpg: true
            jpeg: true
            png: true
            bmp: true
            gif: true
            tiff: true
            webp: true
            jp2: true
            ppm: true
            pgm: true
            pbm: true
            pnm: true
            exr: true
            hdr: true
          video:
            mp4: true
            avi: true
            mov: true
            mkv: true
            wmv: true
            flv: true
            webm: true
            m4v: true
            mpg: true
            mpeg: true
            ts: true
            mts: true
            m2ts: true
            ogv: true
          audio:
            mp3: true
            wav: true
            flac: true
            ogg: true
            m4a: true
            aac: true
            opus: true
            wma: true
            aiff: true
            alac: true
            amr: true
            au: true
          images_raw:
            cr2: true
            nef: true
            arw: true
            dng: true
            raf: true
            rw2: true
            orf: true
            pef: true
            srw: true
            kdc: true
            dcr: true
            mos: true
            mrw: true
            raw: true
            bay: true
            3fr: true
            fff: true
            mef: true
            iiq: true
            rwz: true
            nrw: true
            rwl: true
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

// removed: getProcessingVerbosity()

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
    if (config_["duplicate_linker_check_interval"])
        return config_["duplicate_linker_check_interval"].as<int>();
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

int ServerConfigManager::getProcessingBatchSize() const
{
    std::lock_guard<std::mutex> lock(config_mutex_);
    try
    {
        if (config_["processing"] && config_["processing"]["batch_size"])
        {
            int value = config_["processing"]["batch_size"].as<int>();
            // Validate: must be positive and reasonable
            if (value > 0 && value <= 1000)
            {
                return value;
            }
            else
            {
                Logger::warn("Invalid processing batch_size value: " + std::to_string(value) +
                             ", using default: 100");
            }
        }
    }
    catch (const std::exception &e)
    {
        Logger::warn("Error parsing processing batch_size: " + std::string(e.what()) +
                     ", using default: 100");
    }
    return 100; // Default fallback
}

std::map<std::string, bool> ServerConfigManager::getSupportedFileTypes() const
{
    std::lock_guard<std::mutex> lock(config_mutex_);
    std::map<std::string, bool> supported_types;

    try
    {
        // Categories-only schema: images, video, audio
        if (config_["categories"])
        {
            const YAML::Node &cats = config_["categories"];
            auto add_category = [&](const char *name)
            {
                if (cats[name])
                {
                    for (const auto &file_type : cats[name])
                    {
                        std::string extension = file_type.first.as<std::string>();
                        bool enabled = file_type.second.as<bool>();
                        supported_types[extension] = enabled;
                    }
                }
            };
            add_category("images");
            add_category("video");
            add_category("audio");
        }
    }
    catch (const std::exception &e)
    {
        Logger::error("Error parsing supported file types: " + std::string(e.what()));
    }

    return supported_types;
}

std::map<std::string, bool> ServerConfigManager::getTranscodingFileTypes() const
{
    std::lock_guard<std::mutex> lock(config_mutex_);
    std::map<std::string, bool> transcoding_types;

    try
    {
        // Categories-only schema: images_raw
        if (config_["categories"] && config_["categories"]["images_raw"])
        {
            const YAML::Node &raw = config_["categories"]["images_raw"];
            for (const auto &file_type : raw)
            {
                std::string extension = file_type.first.as<std::string>();
                bool enabled = file_type.second.as<bool>();
                transcoding_types[extension] = enabled;
            }
        }
    }
    catch (const std::exception &e)
    {
        Logger::error("Error parsing transcoding file types: " + std::string(e.what()));
    }

    return transcoding_types;
}

std::vector<std::string> ServerConfigManager::getEnabledFileTypes() const
{
    // Union of all enabled categories plus images_raw (so RAWs are considered supported during scanning)
    std::vector<std::string> enabled_types;
    try
    {
        auto images = getEnabledImageExtensions();
        enabled_types.insert(enabled_types.end(), images.begin(), images.end());
        auto video = getEnabledVideoExtensions();
        enabled_types.insert(enabled_types.end(), video.begin(), video.end());
        auto audio = getEnabledAudioExtensions();
        enabled_types.insert(enabled_types.end(), audio.begin(), audio.end());
        for (const auto &kv : getTranscodingFileTypes())
        {
            if (kv.second)
                enabled_types.push_back(kv.first);
        }
    }
    catch (const std::exception &e)
    {
        Logger::warn("Error building enabled file types: " + std::string(e.what()));
    }
    return enabled_types;
}

bool ServerConfigManager::needsTranscoding(const std::string &file_extension) const
{
    // Convert to lowercase for case-insensitive comparison
    std::string ext = file_extension;
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    // Remove leading dot if present
    if (!ext.empty() && ext[0] == '.')
    {
        ext = ext.substr(1);
    }

    // Check if this extension is in the transcoding configuration
    auto transcoding_types = getTranscodingFileTypes();
    auto it = transcoding_types.find(ext);

    if (it != transcoding_types.end())
    {
        return it->second; // Return the configured value (true/false)
    }

    // If not found in configuration, return false (no transcoding needed)
    return false;
}

uint32_t ServerConfigManager::getDecoderCacheSizeMB() const
{
    std::lock_guard<std::mutex> lock(config_mutex_);
    try
    {
        if (config_["cache"] && config_["cache"]["decoder_cache_size_mb"])
        {
            uint32_t value = config_["cache"]["decoder_cache_size_mb"].as<uint32_t>();
            // Validate: must be positive and reasonable (1MB to 10GB)
            if (value > 0 && value <= 10240)
            {
                return value;
            }
            else
            {
                Logger::warn("Invalid decoder_cache_size_mb value: " + std::to_string(value) +
                             ", using default: 1024");
            }
        }
    }
    catch (const std::exception &e)
    {
        Logger::warn("Error parsing decoder_cache_size_mb: " + std::string(e.what()) +
                     ", using default: 1024");
    }
    return 1024; // Default fallback (1024 MB = 1 GB)
}

int ServerConfigManager::getMaxDecoderThreads() const
{
    std::lock_guard<std::mutex> lock(config_mutex_);
    try
    {
        if (config_["threading"] && config_["threading"]["max_decoder_threads"])
        {
            int value = config_["threading"]["max_decoder_threads"].as<int>();
            // Validate: must be positive and reasonable
            if (value > 0 && value <= 32)
            {
                return value;
            }
            else
            {
                Logger::warn("Invalid max_decoder_threads value: " + std::to_string(value) +
                             ", using default: 4");
            }
        }
    }
    catch (const std::exception &e)
    {
        Logger::warn("Error parsing max_decoder_threads: " + std::string(e.what()) +
                     ", using default: 4");
    }
    return 4; // Default fallback
}

bool ServerConfigManager::getPreProcessQualityStack() const
{
    std::lock_guard<std::mutex> lock(config_mutex_);
    try
    {
        if (config_["pre_process_quality_stack"])
        {
            return config_["pre_process_quality_stack"].as<bool>();
        }
    }
    catch (const std::exception &e)
    {
        Logger::warn("Error parsing pre_process_quality_stack: " + std::string(e.what()) +
                     ", using default: false");
    }
    return false; // Default fallback
}

// Video processing configuration accessors
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

        // Apply to logger immediately
        Logger::init(level);
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
    // Defer special updates to avoid locking the same mutex inside setters
    bool has_new_mode = false;
    DedupMode new_mode_value = DedupMode::BALANCED;
    bool has_new_log_level = false;
    std::string new_log_level_value;
    bool has_new_server_port = false;
    int new_server_port_value = 0;
    bool has_new_auth_secret = false;
    std::string new_auth_secret_value;

    bool config_changed = false;

    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        for (auto it = new_config.begin(); it != new_config.end(); ++it)
        {
            const std::string key = it->first.as<std::string>();

            if (key == "dedup_mode")
            {
                try
                {
                    std::string incoming = it->second.as<std::string>();
                    if (config_["dedup_mode"].as<std::string>() != incoming)
                    {
                        new_mode_value = DedupModes::fromString(incoming);
                        has_new_mode = true;
                    }
                }
                catch (...)
                {
                }
                continue;
            }
            if (key == "log_level")
            {
                try
                {
                    std::string incoming = it->second.as<std::string>();
                    if (config_["log_level"].as<std::string>() != incoming)
                    {
                        new_log_level_value = incoming;
                        has_new_log_level = true;
                    }
                }
                catch (...)
                {
                }
                continue;
            }
            if (key == "server_port")
            {
                try
                {
                    int incoming = it->second.as<int>();
                    if (config_["server_port"].as<int>() != incoming)
                    {
                        new_server_port_value = incoming;
                        has_new_server_port = true;
                    }
                }
                catch (...)
                {
                }
                continue;
            }
            if (key == "auth_secret")
            {
                try
                {
                    std::string incoming = it->second.as<std::string>();
                    if (config_["auth_secret"].as<std::string>() != incoming)
                    {
                        new_auth_secret_value = incoming;
                        has_new_auth_secret = true;
                    }
                }
                catch (...)
                {
                }
                continue;
            }

            // Generic update for all other keys
            if (config_[key])
            {
                YAML::Node old_value = config_[key];
                YAML::Node new_value = it->second;
                if (YAML::Dump(old_value) != YAML::Dump(new_value))
                {
                    config_[key] = new_value;
                    config_changed = true;
                    ConfigEvent event{
                        ConfigEventType::GENERAL_CONFIG_CHANGED,
                        key,
                        old_value,
                        new_value,
                        "Configuration key '" + key + "' updated"};
                    publishEvent(event);
                }
            }
            else
            {
                config_[key] = it->second;
                config_changed = true;
            }
        }
        if (config_changed)
        {
            saveConfigInternal("config.yaml", config_);
        }
    }

    // Apply special keys outside the lock so their setters can lock safely and notify
    if (has_new_mode)
        setDedupMode(new_mode_value);
    if (has_new_log_level)
        setLogLevel(new_log_level_value);
    if (has_new_server_port)
        setServerPort(new_server_port_value);
    if (has_new_auth_secret)
        setAuthSecret(new_auth_secret_value);
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

    // Log to stdout for immediate visibility
    std::cout << "[CONFIG CHANGE DETECTED] " << event.description << std::endl;

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
            // Apply critical settings immediately
            try
            {
                Logger::init(config_["log_level"].as<std::string>());
            }
            catch (...)
            {
            }
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

void ServerConfigManager::startWatching(const std::string &file_path, int interval_seconds)
{
    if (watching_.load())
        return;
    watched_file_path_ = file_path;
    watch_interval_seconds_ = interval_seconds;
    // Initialize last write time
    try
    {
        last_write_time_ = std::filesystem::last_write_time(watched_file_path_);
    }
    catch (...)
    {
        // ignore
    }
    watching_.store(true);
    watcher_thread_ = std::thread([this]()
                                  {
        Logger::info("Starting configuration file watcher for: " + watched_file_path_);
        while (watching_.load()) {
            try {
                auto current = std::filesystem::last_write_time(watched_file_path_);
                if (last_write_time_.time_since_epoch().count() != 0 && current != last_write_time_) {
                    Logger::info("Detected change in configuration file. Reloading...");
                    YAML::Node new_config = YAML::LoadFile(watched_file_path_);
                    if (validateConfig(new_config)) {
                        // Determine diffs and publish events
                        updateConfig(new_config);
                        last_write_time_ = current;
                    } else {
                        Logger::warn("Ignored config change due to validation failure");
                    }
                }
            } catch (const std::exception &e) {
                Logger::warn(std::string("Config watcher error: ") + e.what());
            }
            std::this_thread::sleep_for(std::chrono::seconds(watch_interval_seconds_));
        }
        Logger::info("Configuration file watcher stopped"); });
}

void ServerConfigManager::stopWatching()
{
    if (!watching_.load())
        return;
    watching_.store(false);
    if (watcher_thread_.joinable())
        watcher_thread_.join();
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

    // processing_verbosity removed; no validation

    return true;
}

std::vector<std::string> ServerConfigManager::getEnabledImageExtensions() const
{
    std::lock_guard<std::mutex> lock(config_mutex_);
    std::vector<std::string> image_exts;
    try
    {
        if (config_["categories"] && config_["categories"]["images"])
        {
            const YAML::Node &images = config_["categories"]["images"];
            for (const auto &file_type : images)
            {
                if (file_type.second.as<bool>())
                    image_exts.push_back(file_type.first.as<std::string>());
            }
        }
    }
    catch (const std::exception &e)
    {
        Logger::warn("Error building enabled image extensions: " + std::string(e.what()));
    }
    return image_exts;
}

std::vector<std::string> ServerConfigManager::getEnabledVideoExtensions() const
{
    std::lock_guard<std::mutex> lock(config_mutex_);
    std::vector<std::string> video_exts;
    try
    {
        if (config_["categories"] && config_["categories"]["video"])
        {
            const YAML::Node &video = config_["categories"]["video"];
            for (const auto &file_type : video)
            {
                if (file_type.second.as<bool>())
                    video_exts.push_back(file_type.first.as<std::string>());
            }
        }
    }
    catch (const std::exception &e)
    {
        Logger::warn("Error building enabled video extensions: " + std::string(e.what()));
    }
    return video_exts;
}

std::vector<std::string> ServerConfigManager::getEnabledAudioExtensions() const
{
    std::lock_guard<std::mutex> lock(config_mutex_);
    std::vector<std::string> audio_exts;
    try
    {
        if (config_["categories"] && config_["categories"]["audio"])
        {
            const YAML::Node &audio = config_["categories"]["audio"];
            for (const auto &file_type : audio)
            {
                if (file_type.second.as<bool>())
                    audio_exts.push_back(file_type.first.as<std::string>());
            }
        }
    }
    catch (const std::exception &e)
    {
        Logger::warn("Error building enabled audio extensions: " + std::string(e.what()));
    }
    return audio_exts;
}

YAML::Node ServerConfigManager::getProcessingConfig() const
{
    std::lock_guard<std::mutex> lock(config_mutex_);
    YAML::Node processing_config;

    try
    {
        // Extract processing-related configuration
        if (config_["max_processing_threads"])
            processing_config["max_processing_threads"] = config_["max_processing_threads"];
        if (config_["dedup_mode"])
            processing_config["dedup_mode"] = config_["dedup_mode"];
        if (config_["pre_process_quality_stack"])
            processing_config["pre_process_quality_stack"] = config_["pre_process_quality_stack"];
        if (config_["processing_batch_size"])
            processing_config["processing_batch_size"] = config_["processing_batch_size"];
    }
    catch (const std::exception &e)
    {
        Logger::warn("Error building processing config: " + std::string(e.what()));
    }

    return processing_config;
}

bool ServerConfigManager::validateProcessingConfig(const YAML::Node &config) const
{
    try
    {
        // Validate processing-specific configuration
        if (config["max_processing_threads"])
        {
            int threads = config["max_processing_threads"].as<int>();
            if (threads < 1 || threads > 64)
            {
                Logger::error("Invalid max_processing_threads: " + std::to_string(threads) + ". Must be between 1 and 64.");
                return false;
            }
        }

        if (config["dedup_mode"])
        {
            std::string mode = config["dedup_mode"].as<std::string>();
            if (mode != "FAST" && mode != "BALANCED" && mode != "QUALITY")
            {
                Logger::error("Invalid dedup_mode: " + mode + ". Must be FAST, BALANCED, or QUALITY.");
                return false;
            }
        }

        if (config["pre_process_quality_stack"])
        {
            if (!config["pre_process_quality_stack"].IsScalar())
            {
                Logger::error("Invalid pre_process_quality_stack: must be a boolean value.");
                return false;
            }
        }

        if (config["processing_batch_size"])
        {
            int batch_size = config["processing_batch_size"].as<int>();
            if (batch_size < 1 || batch_size > 10000)
            {
                Logger::error("Invalid processing_batch_size: " + std::to_string(batch_size) + ". Must be between 1 and 10000.");
                return false;
            }
        }

        return true;
    }
    catch (const std::exception &e)
    {
        Logger::error("Error validating processing config: " + std::string(e.what()));
        return false;
    }
}

void ServerConfigManager::updateProcessingConfig(const YAML::Node &config)
{
    std::lock_guard<std::mutex> lock(config_mutex_);

    try
    {
        // Update processing-related configuration
        if (config["max_processing_threads"])
        {
            int old_threads = getMaxProcessingThreads();
            int new_threads = config["max_processing_threads"].as<int>();
            config_["max_processing_threads"] = new_threads;

            if (old_threads != new_threads)
            {
                ConfigEvent event;
                event.type = ConfigEventType::GENERAL_CONFIG_CHANGED;
                event.key = "max_processing_threads";
                event.old_value = YAML::Node(old_threads);
                event.new_value = YAML::Node(new_threads);
                event.description = "Processing thread count changed from " + std::to_string(old_threads) + " to " + std::to_string(new_threads);

                Logger::info("Processing thread count updated: " + std::to_string(old_threads) + " -> " + std::to_string(new_threads));
                publishEvent(event);
            }
        }

        if (config["dedup_mode"])
        {
            DedupMode old_mode_enum = getDedupMode();
            std::string old_mode = DedupModes::getModeName(old_mode_enum);
            std::string new_mode = config["dedup_mode"].as<std::string>();
            config_["dedup_mode"] = new_mode;

            if (old_mode != new_mode)
            {
                ConfigEvent event;
                event.type = ConfigEventType::DEDUP_MODE_CHANGED;
                event.key = "dedup_mode";
                event.old_value = YAML::Node(old_mode);
                event.new_value = YAML::Node(new_mode);
                event.description = "Dedup mode changed from " + old_mode + " to " + new_mode;

                Logger::info("Dedup mode updated: " + old_mode + " -> " + new_mode);
                publishEvent(event);
            }
        }

        if (config["pre_process_quality_stack"])
        {
            bool old_stack = getPreProcessQualityStack();
            bool new_stack = config["pre_process_quality_stack"].as<bool>();
            config_["pre_process_quality_stack"] = new_stack;

            if (old_stack != new_stack)
            {
                ConfigEvent event;
                event.type = ConfigEventType::GENERAL_CONFIG_CHANGED;
                event.key = "pre_process_quality_stack";
                event.old_value = YAML::Node(old_stack);
                event.new_value = YAML::Node(new_stack);
                std::string old_str = old_stack ? "enabled" : "disabled";
                std::string new_str = new_stack ? "enabled" : "disabled";
                event.description = "Pre-process quality stack changed from " + old_str + " to " + new_str;

                Logger::info("Pre-process quality stack updated: " + old_str + " -> " + new_str);
                publishEvent(event);
            }
        }

        if (config["processing_batch_size"])
        {
            int old_batch = getProcessingBatchSize();
            int new_batch = config["processing_batch_size"].as<int>();
            config_["processing_batch_size"] = new_batch;

            if (old_batch != new_batch)
            {
                ConfigEvent event;
                event.type = ConfigEventType::GENERAL_CONFIG_CHANGED;
                event.key = "processing_batch_size";
                event.old_value = YAML::Node(old_batch);
                event.new_value = YAML::Node(new_batch);
                event.description = "Processing batch size changed from " + std::to_string(old_batch) + " to " + std::to_string(new_batch);

                Logger::info("Processing batch size updated: " + std::to_string(old_batch) + " -> " + std::to_string(new_batch));
                publishEvent(event);
            }
        }

        Logger::info("Processing configuration updated successfully");
    }
    catch (const std::exception &e)
    {
        Logger::error("Error updating processing config: " + std::string(e.what()));
        throw;
    }
}

YAML::Node ServerConfigManager::getCacheConfig() const
{
    std::lock_guard<std::mutex> lock(config_mutex_);

    YAML::Node cache_config;

    // Get max cache size from existing config
    if (config_["cache"] && config_["cache"]["decoder_cache_size_mb"])
    {
        cache_config["max_cache_size_gb"] = config_["cache"]["decoder_cache_size_mb"].as<int>() / 1024;
    }
    else
    {
        cache_config["max_cache_size_gb"] = 1; // Default 1 GB
    }

    // Add cache cleanup configuration if it exists
    if (config_["cache_cleanup"])
    {
        cache_config["cache_cleanup"] = config_["cache_cleanup"];
    }
    else
    {
        // Default cache cleanup configuration
        cache_config["cache_cleanup"] = YAML::Load(R"(
            fully_processed_age_days: 7
            partially_processed_age_days: 3
            unprocessed_age_days: 1
            require_all_modes: true
            cleanup_threshold_percent: 80
        )");
    }

    return cache_config;
}

bool ServerConfigManager::validateCacheConfig(const YAML::Node &config) const
{
    try
    {
        if (config["cache_cleanup"])
        {
            auto cleanup = config["cache_cleanup"];

            // Validate age values
            if (cleanup["fully_processed_age_days"] &&
                cleanup["fully_processed_age_days"].as<int>() < 1)
            {
                Logger::warn("Invalid fully_processed_age_days: must be at least 1");
                return false;
            }

            if (cleanup["partially_processed_age_days"] &&
                cleanup["partially_processed_age_days"].as<int>() < 1)
            {
                Logger::warn("Invalid partially_processed_age_days: must be at least 1");
                return false;
            }

            if (cleanup["unprocessed_age_days"] &&
                cleanup["unprocessed_age_days"].as<int>() < 1)
            {
                Logger::warn("Invalid unprocessed_age_days: must be at least 1");
                return false;
            }

            // Validate cleanup threshold
            if (cleanup["cleanup_threshold_percent"])
            {
                int threshold = cleanup["cleanup_threshold_percent"].as<int>();
                if (threshold < 50 || threshold > 95)
                {
                    Logger::warn("Invalid cleanup_threshold_percent: must be between 50 and 95");
                    return false;
                }
            }
        }

        return true;
    }
    catch (const std::exception &e)
    {
        Logger::error("Error validating cache config: " + std::string(e.what()));
        return false;
    }
}

void ServerConfigManager::updateCacheConfig(const YAML::Node &config)
{
    std::lock_guard<std::mutex> lock(config_mutex_);

    try
    {
        bool config_changed = false;

        // Update cache cleanup configuration
        if (config["cache_cleanup"])
        {
            auto cleanup = config["cache_cleanup"];

            if (cleanup["fully_processed_age_days"])
            {
                int old_days = 7; // Default value
                if (config_["cache_cleanup"] && config_["cache_cleanup"]["fully_processed_age_days"])
                {
                    old_days = config_["cache_cleanup"]["fully_processed_age_days"].as<int>();
                }
                int new_days = cleanup["fully_processed_age_days"].as<int>();

                if (!config_["cache_cleanup"])
                {
                    config_["cache_cleanup"] = YAML::Node();
                }
                config_["cache_cleanup"]["fully_processed_age_days"] = new_days;

                if (old_days != new_days)
                {
                    ConfigEvent event;
                    event.type = ConfigEventType::GENERAL_CONFIG_CHANGED;
                    event.key = "fully_processed_age_days";
                    event.old_value = YAML::Node(old_days);
                    event.new_value = YAML::Node(new_days);
                    event.description = "Fully processed age days changed from " + std::to_string(old_days) + " to " + std::to_string(new_days);

                    Logger::info("Fully processed age days updated: " + std::to_string(old_days) + " -> " + std::to_string(new_days));
                    publishEvent(event);
                    config_changed = true;
                }
            }

            if (cleanup["partially_processed_age_days"])
            {
                int old_days = 3; // Default value
                if (config_["cache_cleanup"] && config_["cache_cleanup"]["partially_processed_age_days"])
                {
                    old_days = config_["cache_cleanup"]["partially_processed_age_days"].as<int>();
                }
                int new_days = cleanup["partially_processed_age_days"].as<int>();

                if (!config_["cache_cleanup"])
                {
                    config_["cache_cleanup"] = YAML::Node();
                }
                config_["cache_cleanup"]["partially_processed_age_days"] = new_days;

                if (old_days != new_days)
                {
                    ConfigEvent event;
                    event.type = ConfigEventType::GENERAL_CONFIG_CHANGED;
                    event.key = "partially_processed_age_days";
                    event.old_value = YAML::Node(old_days);
                    event.new_value = YAML::Node(new_days);
                    event.description = "Partially processed age days changed from " + std::to_string(old_days) + " to " + std::to_string(new_days);

                    Logger::info("Partially processed age days updated: " + std::to_string(old_days) + " -> " + std::to_string(new_days));
                    publishEvent(event);
                    config_changed = true;
                }
            }

            if (cleanup["unprocessed_age_days"])
            {
                int old_days = 1; // Default value
                if (config_["cache_cleanup"] && config_["cache_cleanup"]["unprocessed_age_days"])
                {
                    old_days = config_["cache_cleanup"]["unprocessed_age_days"].as<int>();
                }
                int new_days = cleanup["unprocessed_age_days"].as<int>();

                if (!config_["cache_cleanup"])
                {
                    config_["cache_cleanup"] = YAML::Node();
                }
                config_["cache_cleanup"]["unprocessed_age_days"] = new_days;

                if (old_days != new_days)
                {
                    ConfigEvent event;
                    event.type = ConfigEventType::GENERAL_CONFIG_CHANGED;
                    event.key = "unprocessed_age_days";
                    event.old_value = YAML::Node(old_days);
                    event.new_value = YAML::Node(new_days);
                    event.description = "Unprocessed age days changed from " + std::to_string(old_days) + " to " + std::to_string(new_days);

                    Logger::info("Unprocessed age days updated: " + std::to_string(old_days) + " -> " + std::to_string(new_days));
                    publishEvent(event);
                    config_changed = true;
                }
            }

            if (cleanup["require_all_modes"])
            {
                bool old_require = true; // Default value
                if (config_["cache_cleanup"] && config_["cache_cleanup"]["require_all_modes"])
                {
                    old_require = config_["cache_cleanup"]["require_all_modes"].as<bool>();
                }
                bool new_require = cleanup["require_all_modes"].as<bool>();

                if (!config_["cache_cleanup"])
                {
                    config_["cache_cleanup"] = YAML::Node();
                }
                config_["cache_cleanup"]["require_all_modes"] = new_require;

                if (old_require != new_require)
                {
                    ConfigEvent event;
                    event.type = ConfigEventType::GENERAL_CONFIG_CHANGED;
                    event.key = "require_all_modes";
                    event.old_value = YAML::Node(old_require);
                    event.new_value = YAML::Node(new_require);
                    std::string old_str = old_require ? "true" : "false";
                    std::string new_str = new_require ? "true" : "false";
                    event.description = "Require all modes changed from " + old_str + " to " + new_str;

                    Logger::info("Require all modes updated: " + old_str + " -> " + new_str);
                    publishEvent(event);
                    config_changed = true;
                }
            }

            if (cleanup["cleanup_threshold_percent"])
            {
                int old_threshold = 80; // Default value
                if (config_["cache_cleanup"] && config_["cache_cleanup"]["cleanup_threshold_percent"])
                {
                    old_threshold = config_["cache_cleanup"]["cleanup_threshold_percent"].as<int>();
                }
                int new_threshold = cleanup["cleanup_threshold_percent"].as<int>();

                if (!config_["cache_cleanup"])
                {
                    config_["cache_cleanup"] = YAML::Node();
                }
                config_["cache_cleanup"]["cleanup_threshold_percent"] = new_threshold;

                if (old_threshold != new_threshold)
                {
                    ConfigEvent event;
                    event.type = ConfigEventType::GENERAL_CONFIG_CHANGED;
                    event.key = "cleanup_threshold_percent";
                    event.old_value = YAML::Node(old_threshold);
                    event.new_value = YAML::Node(new_threshold);
                    event.description = "Cleanup threshold changed from " + std::to_string(old_threshold) + "% to " + std::to_string(new_threshold) + "%";

                    Logger::info("Cleanup threshold updated: " + std::to_string(old_threshold) + "% -> " + std::to_string(new_threshold) + "%");
                    publishEvent(event);
                    config_changed = true;
                }
            }
        }

        if (config_changed)
        {
            Logger::info("Cache configuration updated successfully");
        }
        else
        {
            Logger::debug("No cache configuration changes detected");
        }
    }
    catch (const std::exception &e)
    {
        Logger::error("Error updating cache config: " + std::string(e.what()));
        throw;
    }
}