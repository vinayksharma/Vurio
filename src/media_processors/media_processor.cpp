#include "media_processors/media_processor.hpp"
#include "media_processors/image_processor.hpp"
#include "config/config_enums.hpp"
#include <Poco/Logger.h>
#include <Poco/LogStream.h>
#include <algorithm>
#include <cctype>

namespace MediaDedup
{
    MediaProcessor::MediaProcessor(std::shared_ptr<UnifiedObservableConfigManager> config_manager)
        : config_manager_(config_manager)
    {
        // Extension mapping will be initialized lazily when first needed
    }

    MediaProcessor::~MediaProcessor()
    {
        shutdown();
    }

    bool MediaProcessor::initialize()
    {
        if (!config_manager_)
        {
            return false;
        }

        // Subscribe to configuration changes
        config_manager_->subscribeToConfigChanges(
            [this](const ConfigChangeEvent &event)
            {
                onConfigChange(event);
            });

        return true;
    }

    void MediaProcessor::shutdown()
    {
        // Unsubscribe from configuration changes
        if (config_manager_)
        {
            config_manager_->unsubscribeFromConfigChanges(
                [this](const ConfigChangeEvent &event)
                {
                    onConfigChange(event);
                });
        }
    }

    bool MediaProcessor::RouteToProcessor(const std::string &file_path)
    {
        std::lock_guard<std::mutex> lock(route_mutex_);

        if (!config_manager_)
        {
            Poco::Logger::get("MediaProcessor").warning("Configuration manager not available");
            return false;
        }

        // Extract file extension
        std::string extension = extractFileExtension(file_path);
        if (extension.empty())
        {
            Poco::Logger::get("MediaProcessor").warning("No file extension found for: " + file_path);
            return false;
        }

        // Get configuration key for this file type
        std::string config_key = getConfigKeyForExtension(extension);
        if (config_key.empty())
        {
            Poco::Logger::get("MediaProcessor").warning("Unsupported file type: " + extension + " for file: " + file_path);
            return false;
        }

        // Check if file type is supported and enabled
        if (!isFileTypeSupported(config_key))
        {
            Poco::Logger::get("MediaProcessor").warning("File type " + extension + " is disabled in configuration for file: " + file_path);
            return false;
        }

        // Get current server mode
        ServerMode server_mode = getCurrentServerMode();

        // Route to appropriate processor based on file type
        FileTypeCategory category = getFileTypeCategory(extension);

        if (category == FileTypeCategory::IMAGE)
        {
            ImageProcessor image_processor;

            switch (server_mode)
            {
            case ServerMode::FAST:
                image_processor.ProcessFast(file_path);
                break;
            case ServerMode::BALANCED:
                image_processor.ProcessBalanced(file_path);
                break;
            case ServerMode::QUALITY:
                image_processor.ProcessQuality(file_path);
                break;
            default:
                image_processor.ProcessFast(file_path);
                break;
            }
        }
        // Future: Add video and audio processors here
        else if (category == FileTypeCategory::VIDEO)
        {
            Poco::Logger::get("MediaProcessor").information("Video processing not yet implemented for: " + file_path);
            return false;
        }
        else if (category == FileTypeCategory::AUDIO)
        {
            Poco::Logger::get("MediaProcessor").information("Audio processing not yet implemented for: " + file_path);
            return false;
        }
        else
        {
            Poco::Logger::get("MediaProcessor").warning("Unsupported file category for: " + file_path);
            return false;
        }

        return true;
    }

    void MediaProcessor::onConfigChange(const ConfigChangeEvent &event)
    {
        // React to configuration changes that affect file type support
        if (event.key.find("media.") == 0)
        {
            Poco::Logger::get("MediaProcessor").debug("Configuration changed for media type: " + event.key);

            // Check if this is a new file type that needs to be added to our mapping
            std::string extension;
            if (event.key.find("media.images.") == 0)
            {
                extension = event.key.substr(12); // Remove "media.images."
                // Remove leading dot if present
                if (!extension.empty() && extension[0] == '.')
                {
                    extension = extension.substr(1);
                }

                // Handle raw formats specially - map both "raw.cr2" and "cr2" to the same config key
                if (extension.find("raw.") == 0)
                {
                    // Map the full raw format name
                    extension_to_config_key_[extension] = event.key;
                    Poco::Logger::get("MediaProcessor").information("Updated extension mapping: " + extension + " -> " + event.key);

                    // Also map just the extension part (e.g., "cr2" for "raw.cr2")
                    std::string short_extension = extension.substr(4); // Remove "raw."
                    extension_to_config_key_[short_extension] = event.key;
                    Poco::Logger::get("MediaProcessor").information("Updated extension mapping: " + short_extension + " -> " + event.key);
                    return; // Skip the general handling below
                }
            }
            else if (event.key.find("media.video.") == 0)
            {
                extension = event.key.substr(11); // Remove "media.video."
                // Remove leading dot if present
                if (!extension.empty() && extension[0] == '.')
                {
                    extension = extension.substr(1);
                }
            }
            else if (event.key.find("media.audio.") == 0)
            {
                extension = event.key.substr(11); // Remove "media.audio."
                // Remove leading dot if present
                if (!extension.empty() && extension[0] == '.')
                {
                    extension = extension.substr(1);
                }
            }

            if (!extension.empty())
            {
                // Add or update the extension mapping
                extension_to_config_key_[extension] = event.key;
                Poco::Logger::get("MediaProcessor").information("Updated extension mapping: " + extension + " -> " + event.key);
            }
        }
    }

    std::string MediaProcessor::extractFileExtension(const std::string &file_path) const
    {
        std::filesystem::path path(file_path);
        std::string extension = path.extension().string();

        if (extension.empty())
        {
            return "";
        }

        // Remove the leading dot and convert to lowercase
        if (extension[0] == '.')
        {
            extension = extension.substr(1);
        }

        std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
        return extension;
    }

    std::string MediaProcessor::getConfigKeyForExtension(const std::string &extension) const
    {
        // Initialize extension mapping if it's empty (lazy initialization)
        if (extension_to_config_key_.empty())
        {
            const_cast<MediaProcessor *>(this)->initializeExtensionMapping();
        }

        auto it = extension_to_config_key_.find(extension);
        if (it != extension_to_config_key_.end())
        {
            return it->second;
        }
        return "";
    }

    bool MediaProcessor::isFileTypeSupported(const std::string &config_key) const
    {
        if (config_key.empty())
        {
            return false;
        }

        return config_manager_->getPropertyValue<bool>(config_key, false);
    }

    ServerMode MediaProcessor::getCurrentServerMode() const
    {
        return config_manager_->getServerMode("server.mode", ServerMode::FAST);
    }

    MediaProcessor::FileTypeCategory MediaProcessor::getFileTypeCategory(const std::string &extension) const
    {
        auto it = extension_to_config_key_.find(extension);
        if (it == extension_to_config_key_.end())
        {
            return FileTypeCategory::UNSUPPORTED;
        }

        const std::string &config_key = it->second;

        if (config_key.find("media.images.") == 0)
        {
            return FileTypeCategory::IMAGE;
        }
        else if (config_key.find("media.video.") == 0)
        {
            return FileTypeCategory::VIDEO;
        }
        else if (config_key.find("media.audio.") == 0)
        {
            return FileTypeCategory::AUDIO;
        }

        return FileTypeCategory::UNSUPPORTED;
    }

    void MediaProcessor::initializeExtensionMapping()
    {
        if (!config_manager_)
        {
            printf("DEBUG: Configuration manager not available for extension mapping\n");
            return;
        }

        // Get all property keys from configuration
        auto all_keys = config_manager_->getAllPropertyKeys();

        Poco::Logger::get("MediaProcessor").debug("Found " + std::to_string(all_keys.size()) + " configuration keys");

        for (const auto &key : all_keys)
        {
            // Check for image formats (including raw)
            if (key.find("media.images.") == 0)
            {
                std::string extension = key.substr(12); // Remove "media.images."
                // Remove leading dot if present
                if (!extension.empty() && extension[0] == '.')
                {
                    extension = extension.substr(1);
                }

                // Handle raw formats specially - map both "raw.cr2" and "cr2" to the same config key
                if (extension.find("raw.") == 0)
                {
                    // Map the full raw format name
                    extension_to_config_key_[extension] = key;
                    Poco::Logger::get("MediaProcessor").debug("Mapped extension: " + extension + " -> " + key);

                    // Also map just the extension part (e.g., "cr2" for "raw.cr2")
                    std::string short_extension = extension.substr(4); // Remove "raw."
                    extension_to_config_key_[short_extension] = key;
                    Poco::Logger::get("MediaProcessor").debug("Mapped extension: " + short_extension + " -> " + key);
                }
                else
                {
                    extension_to_config_key_[extension] = key;
                    Poco::Logger::get("MediaProcessor").debug("Mapped extension: " + extension + " -> " + key);
                }
            }
            // Check for video formats
            else if (key.find("media.video.") == 0)
            {
                std::string extension = key.substr(11); // Remove "media.video."
                // Remove leading dot if present
                if (!extension.empty() && extension[0] == '.')
                {
                    extension = extension.substr(1);
                }
                extension_to_config_key_[extension] = key;
                Poco::Logger::get("MediaProcessor").debug("Mapped extension: " + extension + " -> " + key);
            }
            // Check for audio formats
            else if (key.find("media.audio.") == 0)
            {
                std::string extension = key.substr(11); // Remove "media.audio."
                // Remove leading dot if present
                if (!extension.empty() && extension[0] == '.')
                {
                    extension = extension.substr(1);
                }
                extension_to_config_key_[extension] = key;
                Poco::Logger::get("MediaProcessor").debug("Mapped extension: " + extension + " -> " + key);
            }
        }

        Poco::Logger::get("MediaProcessor").information("Initialized extension mapping with " + std::to_string(extension_to_config_key_.size()) + " file types from configuration");
    }
}
