#include "media_processors/media_processor.hpp"
#include "media_processors/image_processor.hpp"
#include "media_processors/audio_processor.hpp"
#include "media_processors/video_processor.hpp"
#include "config/config_enums.hpp"
#include "database/database_manager.hpp"
#include "database/scanned_files_ops.hpp"
#include "orchestration/thread_pool_manager.hpp"
#include <Poco/Logger.h>
#include <Poco/LogStream.h>
#include <algorithm>
#include <cctype>

namespace MediaDedup
{
    MediaProcessor::MediaProcessor(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                                   std::shared_ptr<DatabaseManager> database_manager,
                                   std::shared_ptr<ThreadPoolManager> thread_pool_manager)
        : config_manager_(config_manager), database_manager_(database_manager), thread_pool_manager_(thread_pool_manager)
    {
        // Extension mapping will be initialized lazily when first needed
    }

    MediaProcessor::~MediaProcessor()
    {
        shutdown();
    }

    bool MediaProcessor::initialize()
    {
        if (!config_manager_ || !thread_pool_manager_)
        {
            return false;
        }

        // Subscribe to configuration changes
        config_manager_->subscribeToConfigChanges(
            [this](const ConfigChangeEvent &event)
            {
                onConfigChange(event);
            });

        // Set up thread pool shares for media processing
        double image_processor_share = config_manager_->getPropertyValue<double>("media.processor.threadPool.share.image_processor", 1.0);
        thread_pool_manager_->setShare("image_processor", image_processor_share);

        double audio_processor_share = config_manager_->getPropertyValue<double>("media.processor.threadPool.share.audio_processor", 1.0);
        thread_pool_manager_->setShare("audio_processor", audio_processor_share);

        double video_processor_share = config_manager_->getPropertyValue<double>("media.processor.threadPool.share.video_processor", 1.0);
        thread_pool_manager_->setShare("video_processor", video_processor_share);

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
        return RouteToProcessorInternal(file_path);
    }

    bool MediaProcessor::RouteToProcessorInternal(const std::string &file_path)
    {
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
            // Submit fire-and-forget lambda to thread pool
            if (thread_pool_manager_)
            {
                // Capture by value for thread safety and to avoid dangling references
                std::string file_path_copy = file_path;
                ServerMode server_mode_copy = server_mode;
                std::shared_ptr<DatabaseManager> db_manager = database_manager_;

                thread_pool_manager_->submit("image_processor", [file_path_copy, server_mode_copy, db_manager]()
                                             {
                    try
                    {
                        Poco::Logger::get("MediaProcessor").debug("Processing file in thread: " + file_path_copy);

                        ImageProcessor image_processor;
                        bool processing_success = false;

                        // Process based on server mode
                        switch (server_mode_copy)
                        {
                        case ServerMode::FAST:
                            processing_success = image_processor.ProcessFast(file_path_copy, *db_manager);
                            break;
                        case ServerMode::BALANCED:
                            processing_success = image_processor.ProcessBalanced(file_path_copy, *db_manager);
                            break;
                        case ServerMode::QUALITY:
                            processing_success = image_processor.ProcessQuality(file_path_copy, *db_manager);
                            break;
                        default:
                            processing_success = image_processor.ProcessFast(file_path_copy, *db_manager);
                            break;
                        }

                        // Update database status within the lambda using connection pool
                        if (processing_success)
                        {
                            ScannedFilesOps::markProcessed(*db_manager, file_path_copy, server_mode_copy, 2);
                            Poco::Logger::get("MediaProcessor").debug("Successfully completed processing file: " + file_path_copy);
                        }
                        else
                        {
                            ScannedFilesOps::markProcessed(*db_manager, file_path_copy, server_mode_copy, -1);
                            Poco::Logger::get("MediaProcessor").warning("Failed to process file: " + file_path_copy);
                        }
                    }
                    catch (const std::exception& e)
                    {
                        Poco::Logger::get("MediaProcessor").error("Exception in image processing thread for file " + file_path_copy + ": " + e.what());
                        // Mark as error in database
                        try
                        {
                            ScannedFilesOps::markProcessed(*db_manager, file_path_copy, server_mode_copy, -1);
                        }
                        catch (...)
                        {
                            Poco::Logger::get("MediaProcessor").error("Failed to mark file as error in database: " + file_path_copy);
                        }
                    }
                    catch (...)
                    {
                        Poco::Logger::get("MediaProcessor").error("Unknown exception in image processing thread for file: " + file_path_copy);
                        // Mark as error in database
                        try
                        {
                            ScannedFilesOps::markProcessed(*db_manager, file_path_copy, server_mode_copy, -1);
                        }
                        catch (...)
                        {
                            Poco::Logger::get("MediaProcessor").error("Failed to mark file as error in database: " + file_path_copy);
                        }
                    } });

                Poco::Logger::get("MediaProcessor").debug("Submitted file for processing: " + file_path);
                return true; // Successfully submitted to thread pool
            }
            else
            {
                Poco::Logger::get("MediaProcessor").error("Thread pool manager not available for file: " + file_path);
                return false;
            }
        }
        else if (category == FileTypeCategory::VIDEO)
        {
            // Submit fire-and-forget lambda to thread pool
            if (thread_pool_manager_)
            {
                // Capture by value for thread safety and to avoid dangling references
                std::string file_path_copy = file_path;
                ServerMode server_mode_copy = server_mode;
                std::shared_ptr<DatabaseManager> db_manager = database_manager_;

                thread_pool_manager_->submit("video_processor", [file_path_copy, server_mode_copy, db_manager]()
                                             {
                    try
                    {
                        Poco::Logger::get("MediaProcessor").debug("Processing file in thread: " + file_path_copy);

                        VideoProcessor video_processor;
                        bool processing_success = false;

                        // Process based on server mode
                        switch (server_mode_copy)
                        {
                        case ServerMode::FAST:
                            processing_success = video_processor.ProcessFast(file_path_copy);
                            break;
                        case ServerMode::BALANCED:
                            processing_success = video_processor.ProcessBalanced(file_path_copy);
                            break;
                        case ServerMode::QUALITY:
                            processing_success = video_processor.ProcessQuality(file_path_copy);
                            break;
                        default:
                            processing_success = video_processor.ProcessFast(file_path_copy);
                            break;
                        }

                        // Update database status within the lambda using connection pool
                        if (processing_success)
                        {
                            ScannedFilesOps::markProcessed(*db_manager, file_path_copy, server_mode_copy, 2);
                            Poco::Logger::get("MediaProcessor").debug("Successfully completed processing file: " + file_path_copy);
                        }
                        else
                        {
                            ScannedFilesOps::markProcessed(*db_manager, file_path_copy, server_mode_copy, -1);
                            Poco::Logger::get("MediaProcessor").warning("Failed to process file: " + file_path_copy);
                        }
                    }
                    catch (const std::exception& e)
                    {
                        Poco::Logger::get("MediaProcessor").error("Exception in video processing thread for file " + file_path_copy + ": " + e.what());
                        // Mark as error in database
                        try
                        {
                            ScannedFilesOps::markProcessed(*db_manager, file_path_copy, server_mode_copy, -1);
                        }
                        catch (...)
                        {
                            Poco::Logger::get("MediaProcessor").error("Failed to mark file as error in database: " + file_path_copy);
                        }
                    }
                    catch (...)
                    {
                        Poco::Logger::get("MediaProcessor").error("Unknown exception in video processing thread for file: " + file_path_copy);
                        // Mark as error in database
                        try
                        {
                            ScannedFilesOps::markProcessed(*db_manager, file_path_copy, server_mode_copy, -1);
                        }
                        catch (...)
                        {
                            Poco::Logger::get("MediaProcessor").error("Failed to mark file as error in database: " + file_path_copy);
                        }
                    } });

                Poco::Logger::get("MediaProcessor").debug("Submitted file for processing: " + file_path);
                return true; // Successfully submitted to thread pool
            }
            else
            {
                Poco::Logger::get("MediaProcessor").error("Thread pool manager not available for file: " + file_path);
                return false;
            }
        }
        else if (category == FileTypeCategory::AUDIO)
        {
            // Submit fire-and-forget lambda to thread pool
            if (thread_pool_manager_)
            {
                // Capture by value for thread safety and to avoid dangling references
                std::string file_path_copy = file_path;
                ServerMode server_mode_copy = server_mode;
                std::shared_ptr<DatabaseManager> db_manager = database_manager_;

                thread_pool_manager_->submit("audio_processor", [file_path_copy, server_mode_copy, db_manager]()
                                             {
                    try
                    {
                        Poco::Logger::get("MediaProcessor").debug("Processing file in thread: " + file_path_copy);

                        AudioProcessor audio_processor;
                        bool processing_success = false;

                        // Process based on server mode
                        switch (server_mode_copy)
                        {
                        case ServerMode::FAST:
                            processing_success = audio_processor.ProcessFast(file_path_copy);
                            break;
                        case ServerMode::BALANCED:
                            processing_success = audio_processor.ProcessBalanced(file_path_copy);
                            break;
                        case ServerMode::QUALITY:
                            processing_success = audio_processor.ProcessQuality(file_path_copy);
                            break;
                        default:
                            processing_success = audio_processor.ProcessFast(file_path_copy);
                            break;
                        }

                        // Update database status within the lambda using connection pool
                        if (processing_success)
                        {
                            ScannedFilesOps::markProcessed(*db_manager, file_path_copy, server_mode_copy, 2);
                            Poco::Logger::get("MediaProcessor").debug("Successfully completed processing file: " + file_path_copy);
                        }
                        else
                        {
                            ScannedFilesOps::markProcessed(*db_manager, file_path_copy, server_mode_copy, -1);
                            Poco::Logger::get("MediaProcessor").warning("Failed to process file: " + file_path_copy);
                        }
                    }
                    catch (const std::exception& e)
                    {
                        Poco::Logger::get("MediaProcessor").error("Exception in audio processing thread for file " + file_path_copy + ": " + e.what());
                        // Mark as error in database
                        try
                        {
                            ScannedFilesOps::markProcessed(*db_manager, file_path_copy, server_mode_copy, -1);
                        }
                        catch (...)
                        {
                            Poco::Logger::get("MediaProcessor").error("Failed to mark file as error in database: " + file_path_copy);
                        }
                    }
                    catch (...)
                    {
                        Poco::Logger::get("MediaProcessor").error("Unknown exception in audio processing thread for file: " + file_path_copy);
                        // Mark as error in database
                        try
                        {
                            ScannedFilesOps::markProcessed(*db_manager, file_path_copy, server_mode_copy, -1);
                        }
                        catch (...)
                        {
                            Poco::Logger::get("MediaProcessor").error("Failed to mark file as error in database: " + file_path_copy);
                        }
                    } });

                Poco::Logger::get("MediaProcessor").debug("Submitted file for processing: " + file_path);
                return true; // Successfully submitted to thread pool
            }
            else
            {
                Poco::Logger::get("MediaProcessor").error("Thread pool manager not available for file: " + file_path);
                return false;
            }
        }
        else
        {
            Poco::Logger::get("MediaProcessor").warning("Unsupported file category for: " + file_path);
            return false;
        }

        return true;
    }

    void MediaProcessor::ProcessMedia()
    {
        Poco::Logger::get("MediaProcessor").information("Media processing thread started");

        std::lock_guard<std::mutex> lock(route_mutex_);

        if (!config_manager_)
        {
            Poco::Logger::get("MediaProcessor").warning("Configuration manager not available for ProcessMedia");
            return;
        }

        if (!database_manager_)
        {
            Poco::Logger::get("MediaProcessor").warning("Database manager not available for ProcessMedia");
            return;
        }

        // Check if media processing is enabled
        bool processing_enabled = config_manager_->getPropertyValue<bool>("media.processor.enabled", true);
        if (!processing_enabled)
        {
            Poco::Logger::get("MediaProcessor").debug("Media processing is disabled in configuration");
            return;
        }

        // Get current server mode
        ServerMode current_mode = getCurrentServerMode();
        std::string mode_str;
        if (current_mode == ServerMode::FAST)
        {
            mode_str = "FAST";
        }
        else if (current_mode == ServerMode::BALANCED)
        {
            mode_str = "BALANCED";
        }
        else if (current_mode == ServerMode::QUALITY)
        {
            mode_str = "QUALITY";
        }
        else
        {
            mode_str = "UNKNOWN";
        }
        Poco::Logger::get("MediaProcessor").information("Processing media files in mode: " + mode_str);

        try
        {
            // Query ALL unprocessed files for current server mode (no batch limit)
            std::vector<ScannedFileRow> unprocessed_files = ScannedFilesOps::listUnprocessed(*database_manager_, current_mode);

            Poco::Logger::get("MediaProcessor").information("Found " + std::to_string(unprocessed_files.size()) + " unprocessed files for current server mode");

            if (unprocessed_files.empty())
            {
                Poco::Logger::get("MediaProcessor").debug("No unprocessed files found, skipping media processing");
                return;
            }

            // Submit each file for processing (fire-and-forget)
            int submitted_count = 0;
            int error_count = 0;

            for (const auto &file_row : unprocessed_files)
            {
                try
                {
                    Poco::Logger::get("MediaProcessor").debug("Submitting file for processing: " + file_row.file_path);

                    // Route file to appropriate processor (fire-and-forget)
                    bool success = RouteToProcessorInternal(file_row.file_path);

                    if (success)
                    {
                        submitted_count++;
                        Poco::Logger::get("MediaProcessor").debug("Successfully submitted file for processing: " + file_row.file_path);
                    }
                    else
                    {
                        // Mark unsupported files as failed (-1) immediately
                        ScannedFilesOps::markProcessed(*database_manager_, file_row.file_path, current_mode, -1);
                        error_count++;
                        Poco::Logger::get("MediaProcessor").warning("Failed to submit file for processing: " + file_row.file_path);
                    }
                }
                catch (const std::exception &e)
                {
                    // Log error and continue with next file
                    Poco::Logger::get("MediaProcessor").error("Exception while submitting file " + file_row.file_path + ": " + e.what());
                    error_count++;
                }
            }

            Poco::Logger::get("MediaProcessor").information("Media processing submission completed - Submitted: " + std::to_string(submitted_count) + ", Errors: " + std::to_string(error_count));
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("MediaProcessor").error("Exception in ProcessMedia: " + std::string(e.what()));
        }

        Poco::Logger::get("MediaProcessor").information("Media processing thread finished");
    }

    void MediaProcessor::onConfigChange(const ConfigChangeEvent &event)
    {
        // React to thread pool configuration changes
        if (event.key == "media.processor.threadPool.share.image_processor")
        {
            if (thread_pool_manager_)
            {
                double new_share = config_manager_->getPropertyValue<double>(event.key, 1.0);
                thread_pool_manager_->setShare("image_processor", new_share);
                Poco::Logger::get("MediaProcessor").information("Updated thread pool share for image_processor: " + std::to_string(new_share));
            }
            return;
        }
        else if (event.key == "media.processor.threadPool.share.audio_processor")
        {
            if (thread_pool_manager_)
            {
                double new_share = config_manager_->getPropertyValue<double>(event.key, 1.0);
                thread_pool_manager_->setShare("audio_processor", new_share);
                Poco::Logger::get("MediaProcessor").information("Updated thread pool share for audio_processor: " + std::to_string(new_share));
            }
            return;
        }
        else if (event.key == "media.processor.threadPool.share.video_processor")
        {
            if (thread_pool_manager_)
            {
                double new_share = config_manager_->getPropertyValue<double>(event.key, 1.0);
                thread_pool_manager_->setShare("video_processor", new_share);
                Poco::Logger::get("MediaProcessor").information("Updated thread pool share for video_processor: " + std::to_string(new_share));
            }
            return;
        }

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

    std::set<std::string> MediaProcessor::getAllSupportedMediaExtensions() const
    {
        std::set<std::string> supported_extensions;

        if (!config_manager_)
        {
            Poco::Logger::get("MediaProcessor").warning("Configuration manager not available for getting supported extensions");
            return supported_extensions;
        }

        // Get all property keys from configuration
        auto all_keys = config_manager_->getAllPropertyKeys();

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

                // Handle raw formats specially - add both "raw.cr2" and "cr2"
                if (extension.find("raw.") == 0)
                {
                    // Add the full raw format name
                    supported_extensions.insert(extension);

                    // Also add just the extension part (e.g., "cr2" for "raw.cr2")
                    std::string short_extension = extension.substr(4); // Remove "raw."
                    supported_extensions.insert(short_extension);
                }
                else
                {
                    supported_extensions.insert(extension);
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
                supported_extensions.insert(extension);
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
                supported_extensions.insert(extension);
            }
        }

        Poco::Logger::get("MediaProcessor").debug("Found " + std::to_string(supported_extensions.size()) + " supported media extensions from configuration");
        return supported_extensions;
    }

    int MediaProcessor::clearProcessingFlags()
    {
        std::lock_guard<std::mutex> lock(route_mutex_);

        if (!database_manager_)
        {
            Poco::Logger::get("MediaProcessor").warning("Database manager not available for clearing processing flags");
            return -1;
        }

        Poco::Logger::get("MediaProcessor").information("Clearing all processing flags from 1 (picked up for processing) to 0 (ready to be processed)");

        try
        {
            int cleared_count = ScannedFilesOps::clearProcessingFlags(*database_manager_);

            if (cleared_count >= 0)
            {
                Poco::Logger::get("MediaProcessor").information("Successfully cleared processing flags for " + std::to_string(cleared_count) + " files");
            }
            else
            {
                Poco::Logger::get("MediaProcessor").error("Failed to clear processing flags - database operation failed");
            }

            return cleared_count;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("MediaProcessor").error("Exception while clearing processing flags: " + std::string(e.what()));
            return -1;
        }
    }
}
