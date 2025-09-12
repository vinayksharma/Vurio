#pragma once

#include <string>
#include <filesystem>
#include <functional>
#include <memory>
#include <unordered_map>
#include <any>
#include "config/yaml_serializer.hpp"

namespace MediaDedup
{

    /**
     * @brief Configuration file change callback type
     */
    using FileChangeCallback = std::function<void(const std::string &)>;

    /**
     * @brief Manages configuration file operations
     *
     * Handles loading, saving, and monitoring of configuration files
     * with proper error handling and directory management.
     */
    class ConfigFileManager
    {
    public:
        /**
         * @brief Constructor
         * @param config_file_path Path to the configuration file
         */
        explicit ConfigFileManager(const std::string &config_file_path);

        /**
         * @brief Destructor
         */
        ~ConfigFileManager() = default;

        // File operations
        /**
         * @brief Load configuration from file
         * @return true if successful, false otherwise
         */
        bool loadConfiguration();

        /**
         * @brief Save configuration to file
         * @param properties Properties to save
         * @return true if successful, false otherwise
         */
        bool saveConfiguration(const std::unordered_map<std::string, std::any> &properties);

        /**
         * @brief Reload configuration from file
         * @return true if successful, false otherwise
         */
        bool reloadConfiguration();

        /**
         * @brief Check if configuration file exists
         * @return true if file exists, false otherwise
         */
        bool fileExists() const;

        /**
         * @brief Get the configuration file path
         * @return Configuration file path
         */
        std::string getConfigFilePath() const { return config_file_path_; }

        /**
         * @brief Set file change callback
         * @param callback Callback to invoke when file changes
         */
        void setFileChangeCallback(FileChangeCallback callback) { file_change_callback_ = callback; }

        /**
         * @brief Get last file modification time
         * @return Last modification time
         */
        std::filesystem::file_time_type getLastModificationTime() const { return last_file_modification_; }

        /**
         * @brief Update last modification time to current file time
         */
        void updateLastModificationTime();

        /**
         * @brief Check if file has changed since last check
         * @return true if file has changed, false otherwise
         */
        bool hasFileChanged() const;

        /**
         * @brief Get parsed properties from file
         * @return Map of properties loaded from file
         */
        std::unordered_map<std::string, std::any> getLoadedProperties() const;

        /**
         * @brief Get validation errors
         * @return Vector of validation error messages
         */
        std::vector<std::string> getValidationErrors() const { return validation_errors_; }

        /**
         * @brief Clear validation errors
         */
        void clearValidationErrors() { validation_errors_.clear(); }

        /**
         * @brief Check if configuration is valid
         * @return true if valid, false otherwise
         */
        bool isValid() const { return valid_; }

    private:
        std::string config_file_path_;
        std::filesystem::file_time_type last_file_modification_;
        YamlConfigSerializer yaml_serializer_;
        FileChangeCallback file_change_callback_;

        std::atomic<bool> valid_;
        std::vector<std::string> validation_errors_;

        /**
         * @brief Create configuration directory if it doesn't exist
         * @return true if successful, false otherwise
         */
        bool ensureConfigDirectoryExists();

        /**
         * @brief Parse YAML file and return properties
         * @return Map of properties from file
         */
        std::unordered_map<std::string, std::any> parseYamlFile();

        /**
         * @brief Add validation error
         * @param error Error message to add
         */
        void addValidationError(const std::string &error);

        /**
         * @brief Notify file change callback
         * @param file_path Path of changed file
         */
        void notifyFileChange(const std::string &file_path);
    };

} // namespace MediaDedup
