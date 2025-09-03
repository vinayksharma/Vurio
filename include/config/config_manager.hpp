#pragma once

#include <Poco/Util/Application.h>
#include <Poco/Util/PropertyFileConfiguration.h>
#include <Poco/AutoPtr.h>
#include <string>
#include <memory>

namespace MediaDedup {

/**
 * @brief Configuration manager for the media deduplication server
 * 
 * This class handles all configuration aspects using Poco libraries:
 * - Loading configuration from files
 * - Environment variable overrides
 * - Default value management
 * - Configuration validation
 */
class ConfigManager {
public:
    /**
     * @brief Constructor
     * @param config_file_path Path to the configuration file
     */
    explicit ConfigManager(const std::string& config_file_path = "config/config.yaml");
    
    /**
     * @brief Destructor
     */
    ~ConfigManager() = default;
    
    /**
     * @brief Load configuration from file
     * @param config_file_path Path to configuration file
     * @return true if successful, false otherwise
     */
    bool loadConfiguration(const std::string& config_file_path);
    
    /**
     * @brief Get string configuration value
     * @param key Configuration key
     * @param default_value Default value if key not found
     * @return Configuration value
     */
    std::string getString(const std::string& key, const std::string& default_value = "") const;
    
    /**
     * @brief Get integer configuration value
     * @param key Configuration key
     * @param default_value Default value if key not found
     * @return Configuration value
     */
    int getInt(const std::string& key, int default_value = 0) const;
    
    /**
     * @brief Get boolean configuration value
     * @param key Configuration key
     * @param default_value Default value if key not found
     * @return Configuration value
     */
    bool getBool(const std::string& key, bool default_value = false) const;
    
    /**
     * @brief Get double configuration value
     * @param key Configuration key
     * @param default_value Default value if key not found
     * @return Configuration value
     */
    double getDouble(const std::string& key, double default_value = 0.0) const;
    
    /**
     * @brief Set configuration value
     * @param key Configuration key
     * @param value Configuration value
     */
    void setValue(const std::string& key, const std::string& value);
    
    /**
     * @brief Check if configuration key exists
     * @param key Configuration key
     * @return true if key exists, false otherwise
     */
    bool hasKey(const std::string& key) const;
    
    /**
     * @brief Validate configuration
     * @return true if configuration is valid, false otherwise
     */
    bool validateConfiguration() const;
    
    /**
     * @brief Get database configuration
     * @return Database configuration section
     */
    std::unique_ptr<ConfigManager> getDatabaseConfig() const;
    
    /**
     * @brief Get server configuration
     * @return Server configuration section
     */
    std::unique_ptr<ConfigManager> getServerConfig() const;

private:
    Poco::AutoPtr<Poco::Util::PropertyFileConfiguration> config_;
    std::string config_file_path_;
    
    /**
     * @brief Load default configuration
     */
    void loadDefaultConfiguration();
    
    /**
     * @brief Validate required configuration keys
     * @return true if all required keys are present, false otherwise
     */
    bool validateRequiredKeys() const;
};

} // namespace MediaDedup
