#include "config/file_manager.hpp"
#include <filesystem>
#include <iostream>

namespace MediaDedup
{

    ConfigFileManager::ConfigFileManager(const std::string &config_file_path)
        : config_file_path_(config_file_path),
          yaml_serializer_(config_file_path),
          valid_(false)
    {
        try
        {
            if (std::filesystem::exists(config_file_path_))
            {
                last_file_modification_ = std::filesystem::last_write_time(config_file_path_);
            }
            else
            {
                last_file_modification_ = std::filesystem::file_time_type::min();
            }
        }
        catch (...)
        {
            last_file_modification_ = std::filesystem::file_time_type::min();
        }
    }

    bool ConfigFileManager::loadConfiguration()
    {
        try
        {
            if (!std::filesystem::exists(config_file_path_))
            {
                // File doesn't exist, create with defaults
                return ensureConfigDirectoryExists();
            }

            auto properties = parseYamlFile();
            if (!properties.empty())
            {
                valid_ = true;
                clearValidationErrors();
                updateLastModificationTime();
                return true;
            }

            return false;
        }
        catch (const std::exception &e)
        {
            addValidationError("Failed to load configuration: " + std::string(e.what()));
            valid_ = false;
            return false;
        }
    }

    bool ConfigFileManager::saveConfiguration(const std::unordered_map<std::string, std::any> &properties)
    {
        try
        {
            if (!ensureConfigDirectoryExists())
            {
                return false;
            }

            bool success = yaml_serializer_.serializeToYamlFile(properties);
            if (success)
            {
                updateLastModificationTime();
                valid_ = true;
                clearValidationErrors();
            }
            else
            {
                addValidationError("Failed to serialize configuration to YAML file");
                valid_ = false;
            }

            return success;
        }
        catch (const std::exception &e)
        {
            addValidationError("Failed to save configuration: " + std::string(e.what()));
            valid_ = false;
            return false;
        }
    }

    bool ConfigFileManager::reloadConfiguration()
    {
        if (loadConfiguration())
        {
            notifyFileChange(config_file_path_);
            return true;
        }
        return false;
    }

    bool ConfigFileManager::fileExists() const
    {
        return std::filesystem::exists(config_file_path_);
    }

    void ConfigFileManager::updateLastModificationTime()
    {
        try
        {
            if (std::filesystem::exists(config_file_path_))
            {
                last_file_modification_ = std::filesystem::last_write_time(config_file_path_);
            }
        }
        catch (...)
        {
            // ignore
        }
    }

    bool ConfigFileManager::hasFileChanged() const
    {
        try
        {
            if (!std::filesystem::exists(config_file_path_))
            {
                return false;
            }

            auto current_time = std::filesystem::last_write_time(config_file_path_);
            return current_time > last_file_modification_;
        }
        catch (const std::exception &)
        {
            return false;
        }
    }

    std::unordered_map<std::string, std::any> ConfigFileManager::getLoadedProperties() const
    {
        try
        {
            return yaml_serializer_.parseYamlFile();
        }
        catch (const std::exception &)
        {
            return {};
        }
    }

    bool ConfigFileManager::ensureConfigDirectoryExists()
    {
        try
        {
            auto config_dir = std::filesystem::path(config_file_path_).parent_path();
            if (!config_dir.empty() && !std::filesystem::exists(config_dir))
            {
                std::filesystem::create_directories(config_dir);
            }
            return true;
        }
        catch (const std::exception &e)
        {
            addValidationError("Failed to create config directory: " + std::string(e.what()));
            return false;
        }
    }

    std::unordered_map<std::string, std::any> ConfigFileManager::parseYamlFile()
    {
        try
        {
            return yaml_serializer_.parseYamlFile();
        }
        catch (const std::exception &e)
        {
            addValidationError("YAML parsing failed: " + std::string(e.what()));
            valid_ = false;
            return {};
        }
    }

    void ConfigFileManager::addValidationError(const std::string &error)
    {
        validation_errors_.push_back(error);
    }

    void ConfigFileManager::notifyFileChange(const std::string &file_path)
    {
        if (file_change_callback_)
        {
            file_change_callback_(file_path);
        }
    }

} // namespace MediaDedup
