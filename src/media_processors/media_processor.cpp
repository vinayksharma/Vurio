#include "media_processors/media_processor.hpp"
#include "media_processors/image_processor.hpp"
#include "media_processors/audio_processor.hpp"
#include "media_processors/video_processor.hpp"
#include "media_processors/image/backends/raw_file_detector.hpp"
#include "media_processors/image/backends/transcoding_pipeline.hpp"
#include "filesmanager/disk_cache.hpp"
#include "config/config_enums.hpp"
#include "database/database_manager.hpp"
#include "database/scanned_files_ops.hpp"
#include "orchestration/thread_pool_manager.hpp"
#include <Poco/Logger.h>
#include <Poco/LogStream.h>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sys/resource.h>

namespace MediaDedup
{
    MediaProcessor::MediaProcessor(std::shared_ptr<UnifiedObservableConfigManager> config_manager,
                                   std::shared_ptr<DatabaseManager> database_manager,
                                   std::shared_ptr<ThreadPoolManager> thread_pool_manager)
        : config_manager_(config_manager), database_manager_(database_manager), thread_pool_manager_(thread_pool_manager)
    {
        // Extension mapping will be initialized lazily when first needed
        // Initialize disk cache
        disk_cache_ = std::make_shared<DiskCache>(config_manager);
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

        // Set up unified thread pool share for media processing
        double media_processor_share = config_manager_->getPropertyValue<double>("media.processor.threadPool.share.media_processor", 1.0);
        thread_pool_manager_->setShare("media_processor", media_processor_share);

        // Initialize unified queue size limit from configuration
        // Read as int (like other integer config values) and convert to size_t
        max_processing_queue_size_ = static_cast<size_t>(config_manager_->getPropertyValue<int>("media.processor.maxQueueSize", 10000));

        Poco::Logger::get("MediaProcessor").information("Initialized unified processing queue size limit: %u", static_cast<unsigned int>(max_processing_queue_size_));

        // Initialize disk cache
        if (!disk_cache_->initialize())
        {
            Poco::Logger::get("MediaProcessor").error("Failed to initialize disk cache");
            return false;
        }

        Poco::Logger::get("MediaProcessor").information("MediaProcessor initialized with disk cache support");

        return true;
    }

    void MediaProcessor::shutdown()
    {
        Poco::Logger &logger = Poco::Logger::get("MediaProcessor");
        logger.information("Shutting down MediaProcessor...");

        try
        {
            // Clear processing flags to reset any files that were in processing state
            int cleared_count = clearProcessingFlags();
            if (cleared_count > 0)
            {
                logger.information("Cleared processing flags for %d files during shutdown", cleared_count);
            }

            // Clear the disk cache
            if (disk_cache_)
            {
                logger.debug("Clearing disk cache during shutdown...");
                disk_cache_->clearCache();
                logger.debug("Disk cache cleared successfully");
            }

            // Unsubscribe from configuration changes
            if (config_manager_)
            {
                config_manager_->unsubscribeFromConfigChanges(
                    [this](const ConfigChangeEvent &event)
                    {
                        onConfigChange(event);
                    });
                logger.debug("Unsubscribed from configuration changes");
            }

            logger.information("MediaProcessor shutdown complete");
        }
        catch (const std::exception &e)
        {
            logger.error("Exception during MediaProcessor shutdown: %s", e.what());
        }
        catch (...)
        {
            logger.error("Unknown exception during MediaProcessor shutdown");
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
                // Check queue capacity before submitting to prevent memory buildup
                if (!thread_pool_manager_->canSubmit("media_processor", max_processing_queue_size_))
                {
                    Poco::Logger::get("MediaProcessor").trace("Media processor queue at capacity (%u), skipping image file: %s", static_cast<unsigned int>(max_processing_queue_size_), file_path);
                    // Mark file as skipped in database to avoid reprocessing
                    try
                    {
                        ScannedFilesOps::markProcessed(*database_manager_, file_path, server_mode, -2); // -2 = skipped due to backpressure
                    }
                    catch (...)
                    {
                        Poco::Logger::get("MediaProcessor").error("Failed to mark file as skipped in database: %s", file_path);
                    }
                    return false; // Indicate that processing was skipped
                }

                // Mark file as queued for processing
                try
                {
                    ScannedFilesOps::markProcessed(*database_manager_, file_path, server_mode, -99); // -99 = queued
                }
                catch (...)
                {
                    Poco::Logger::get("MediaProcessor").error("Failed to mark file as queued in database: %s", file_path);
                }

                // Capture by value for thread safety and to avoid dangling references
                std::string file_path_copy = file_path;
                ServerMode server_mode_copy = server_mode;
                std::shared_ptr<DatabaseManager> db_manager = database_manager_;
                std::shared_ptr<UnifiedObservableConfigManager> config_manager = config_manager_;
                std::shared_ptr<DiskCache> disk_cache = disk_cache_;

                thread_pool_manager_->submit("media_processor", [file_path_copy, server_mode_copy, db_manager, config_manager, disk_cache]()
                {
                    try
                    {
                        Poco::Logger::get("MediaProcessor").debug("Processing file in thread: " + file_path_copy);

                        // Mark file as in progress
                        ScannedFilesOps::markProcessed(*db_manager, file_path_copy, server_mode_copy, 1); // 1 = in progress

                        // Copy file to cache
                        std::string cached_path;
                        if (!disk_cache->copyToCache(file_path_copy, cached_path))
                        {
                            Poco::Logger::get("MediaProcessor").error("Failed to copy file to cache: " + file_path_copy);
                            ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, server_mode_copy, -6); // Cache operation error
                            return;
                        }

                        // Check if file needs transcoding
                        std::string processing_file_path = cached_path;
                        bool needs_transcoding = RawFileDetector::IsRawFile(cached_path);
                        
                        if (needs_transcoding)
                        {
                            Poco::Logger::get("MediaProcessor").information("Raw file detected, transcoding in cache: %s", cached_path);
                            
                            // Log memory usage before transcoding
                            struct rusage usage;
                            if (getrusage(RUSAGE_SELF, &usage) == 0)
                            {
                                Poco::Logger::get("MediaProcessor").debug("Memory usage before transcoding: %ld KB", usage.ru_maxrss);
                            }
                            
                            // Transcode to a new file in cache
                            std::string transcoded_filename;
                            TranscodingConfig transcode_config = TranscodingPipeline::GetConfigFromManager(config_manager);
                            
                            // Get cache directory from disk_cache for transcoding
                            std::string cache_directory = disk_cache->getCacheLocation();
                            if (TranscodingPipeline::TranscodeToFile(cached_path, transcoded_filename, transcode_config, cache_directory))
                            {
                                // Validate transcoded file exists and has content
                                if (!std::filesystem::exists(transcoded_filename))
                                {
                                    Poco::Logger::get("MediaProcessor").error("Transcoded file was not created: %s", transcoded_filename);
                                    ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, server_mode_copy, -1);
                                    disk_cache->deleteFromCacheImmediately(cached_path);
                                    return;
                                }

                                auto transcoded_size = std::filesystem::file_size(transcoded_filename);
                                if (transcoded_size == 0)
                                {
                                    Poco::Logger::get("MediaProcessor").error("Transcoded file is empty: %s", transcoded_filename);
                                    std::filesystem::remove(transcoded_filename);
                                    ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, server_mode_copy, -1);
                                    disk_cache->deleteFromCacheImmediately(cached_path);
                                    return;
                                }

                                Poco::Logger::get("MediaProcessor").debug("Transcoded file created successfully - size: %zu bytes", transcoded_size);

                                // Log memory usage after transcoding
                                struct rusage usage_after;
                                if (getrusage(RUSAGE_SELF, &usage_after) == 0)
                                {
                                    Poco::Logger::get("MediaProcessor").debug("Memory usage after transcoding: %ld KB", usage_after.ru_maxrss);
                                }

                                // Transcoded file is already in cache directory, no need to copy
                                Poco::Logger::get("MediaProcessor").information("Successfully transcoded file: %s -> %s", cached_path, transcoded_filename);
                                
                                // Delete original RAW file from cache
                                disk_cache->deleteFromCacheImmediately(cached_path);
                                
                                // Use transcoded file for processing (already in cache)
                                processing_file_path = transcoded_filename;
                            }
                            else
                            {
                                Poco::Logger::get("MediaProcessor").error("Failed to transcode raw file: %s (this may be due to unsupported RAW format or corrupted file)", cached_path);
                                // Mark as processing error since RAW files cannot be processed without transcoding
                                ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, server_mode_copy, -1);
                                // Clean up the original cached file
                                disk_cache->deleteFromCacheImmediately(cached_path);
                                return;
                            }
                        }

                        // Mark file as in use
                        disk_cache->markFileInUse(processing_file_path);

                        try
                        {
                            ImageProcessor image_processor;
                            bool processing_success = false;

                            // Process based on server mode using cached file
                            // Pass both processing file path and original source file path
                            switch (server_mode_copy)
                            {
                            case ServerMode::FAST:
                                processing_success = image_processor.ProcessFast(processing_file_path, file_path_copy, *db_manager, config_manager);
                                break;
                            case ServerMode::BALANCED:
                                processing_success = image_processor.ProcessBalanced(processing_file_path, file_path_copy, *db_manager, config_manager);
                                break;
                            case ServerMode::QUALITY:
                                processing_success = image_processor.ProcessQuality(processing_file_path, file_path_copy, *db_manager, config_manager);
                                break;
                            default:
                                processing_success = image_processor.ProcessFast(processing_file_path, file_path_copy, *db_manager, config_manager);
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
                                ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, server_mode_copy, -1); // General processing error
                                Poco::Logger::get("MediaProcessor").warning("Failed to process file: " + file_path_copy);
                            }
                        }
                        catch (...)
                        {
                            // Clean up cache file before re-throwing
                            disk_cache->markFileNotInUse(processing_file_path);
                            disk_cache->deleteFromCacheImmediately(processing_file_path);
                            throw;
                        }

                        // Use RAII pattern for cleanup - runs after all processing is complete
                        struct CacheCleanup {
                            std::shared_ptr<DiskCache> cache;
                            std::string path;
                            CacheCleanup(std::shared_ptr<DiskCache> c, const std::string& p) : cache(c), path(p) {}
                            ~CacheCleanup() {
                                cache->markFileNotInUse(path);
                                cache->deleteFromCacheImmediately(path);
                            }
                        } cleanup(disk_cache, processing_file_path);
                    }
                    catch (const std::bad_alloc& e)
                    {
                        Poco::Logger::get("MediaProcessor").error("Memory allocation error processing file " + file_path_copy + ": " + e.what());
                        ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, server_mode_copy, -4); // Memory allocation error
                    }
                    catch (const std::filesystem::filesystem_error& e)
                    {
                        Poco::Logger::get("MediaProcessor").error("File system error processing file " + file_path_copy + ": " + e.what());
                        ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, server_mode_copy, -3); // File access error
                    }
                    catch (const std::runtime_error& e)
                    {
                        std::string error_msg = e.what();
                        // Check for network-related error messages
                        if (error_msg.find("network") != std::string::npos || 
                            error_msg.find("connection") != std::string::npos ||
                            error_msg.find("timeout") != std::string::npos)
                        {
                            Poco::Logger::get("MediaProcessor").error("Network error processing file " + file_path_copy + ": " + error_msg);
                            ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, server_mode_copy, -5); // Network-related error
                        }
                        else
                        {
                            Poco::Logger::get("MediaProcessor").error("Runtime error processing file " + file_path_copy + ": " + error_msg);
                            ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, server_mode_copy, -1); // General error
                        }
                    }
                    catch (const std::exception& e)
                    {
                        Poco::Logger::get("MediaProcessor").error("Exception in image processing thread for file " + file_path_copy + ": " + e.what());
                        ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, server_mode_copy, -1); // General error
                    }
                    catch (...)
                    {
                        Poco::Logger::get("MediaProcessor").error("Unknown exception in image processing thread for file: " + file_path_copy);
                        ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, server_mode_copy, -1); // General error
                    } }, file_path_copy);

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
                // Check queue capacity before submitting to prevent memory buildup
                if (!thread_pool_manager_->canSubmit("media_processor", max_processing_queue_size_))
                {
                    Poco::Logger::get("MediaProcessor").trace("Media processor queue at capacity (%u), skipping video file: %s", static_cast<unsigned int>(max_processing_queue_size_), file_path);
                    // Mark file as skipped in database to avoid reprocessing
                    try
                    {
                        ScannedFilesOps::markProcessed(*database_manager_, file_path, server_mode, -2); // -2 = skipped due to backpressure
                    }
                    catch (...)
                    {
                        Poco::Logger::get("MediaProcessor").error("Failed to mark file as skipped in database: %s", file_path);
                    }
                    return false; // Indicate that processing was skipped
                }

                // Capture by value for thread safety and to avoid dangling references
                std::string file_path_copy = file_path;
                ServerMode server_mode_copy = server_mode;
                std::shared_ptr<DatabaseManager> db_manager = database_manager_;

                thread_pool_manager_->submit("media_processor", [file_path_copy, server_mode_copy, db_manager]()
                                             {
                    try
                    {
                        Poco::Logger::get("MediaProcessor").debug("Processing file in thread: " + file_path_copy);

                        // Mark file as in progress
                        ScannedFilesOps::markProcessed(*db_manager, file_path_copy, server_mode_copy, 1); // 1 = in progress

                        // Check if file exists and is accessible (file access error detection)
                        if (!std::filesystem::exists(file_path_copy))
                        {
                            Poco::Logger::get("MediaProcessor").error("File not found: " + file_path_copy);
                            ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, server_mode_copy, -3); // File access error
                            return;
                        }

                        if (!std::filesystem::is_regular_file(file_path_copy))
                        {
                            Poco::Logger::get("MediaProcessor").error("File is not a regular file: " + file_path_copy);
                            ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, server_mode_copy, -3); // File access error
                            return;
                        }

                        VideoProcessor video_processor;
                        bool processing_success = false;

                        // Process based on server mode (media loading happens here on-demand)
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
                            ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, server_mode_copy, -1); // General processing error
                            Poco::Logger::get("MediaProcessor").warning("Failed to process file: " + file_path_copy);
                        }
                    }
                    catch (const std::bad_alloc& e)
                    {
                        Poco::Logger::get("MediaProcessor").error("Memory allocation error processing file " + file_path_copy + ": " + e.what());
                        ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, server_mode_copy, -4); // Memory allocation error
                    }
                    catch (const std::filesystem::filesystem_error& e)
                    {
                        Poco::Logger::get("MediaProcessor").error("File system error processing file " + file_path_copy + ": " + e.what());
                        ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, server_mode_copy, -3); // File access error
                    }
                    catch (const std::runtime_error& e)
                    {
                        std::string error_msg = e.what();
                        // Check for network-related error messages
                        if (error_msg.find("network") != std::string::npos || 
                            error_msg.find("connection") != std::string::npos ||
                            error_msg.find("timeout") != std::string::npos)
                        {
                            Poco::Logger::get("MediaProcessor").error("Network error processing file " + file_path_copy + ": " + error_msg);
                            ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, server_mode_copy, -5); // Network-related error
                        }
                        else
                        {
                            Poco::Logger::get("MediaProcessor").error("Runtime error processing file " + file_path_copy + ": " + error_msg);
                            ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, server_mode_copy, -1); // General error
                        }
                    }
                    catch (const std::exception& e)
                    {
                        Poco::Logger::get("MediaProcessor").error("Exception in video processing thread for file " + file_path_copy + ": " + e.what());
                        ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, server_mode_copy, -1); // General error
                    }
                    catch (...)
                    {
                        Poco::Logger::get("MediaProcessor").error("Unknown exception in video processing thread for file: " + file_path_copy);
                        ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, server_mode_copy, -1); // General error
                    } }, file_path_copy);

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
                // Check queue capacity before submitting to prevent memory buildup
                if (!thread_pool_manager_->canSubmit("media_processor", max_processing_queue_size_))
                {
                    Poco::Logger::get("MediaProcessor").trace("Media processor queue at capacity (%u), skipping audio file: %s", static_cast<unsigned int>(max_processing_queue_size_), file_path);
                    // Mark file as skipped in database to avoid reprocessing
                    try
                    {
                        ScannedFilesOps::markProcessed(*database_manager_, file_path, server_mode, -2); // -2 = skipped due to backpressure
                    }
                    catch (...)
                    {
                        Poco::Logger::get("MediaProcessor").error("Failed to mark file as skipped in database: %s", file_path);
                    }
                    return false; // Indicate that processing was skipped
                }

                // Capture by value for thread safety and to avoid dangling references
                std::string file_path_copy = file_path;
                ServerMode server_mode_copy = server_mode;
                std::shared_ptr<DatabaseManager> db_manager = database_manager_;

                thread_pool_manager_->submit("media_processor", [file_path_copy, server_mode_copy, db_manager]()
                                             {
                    try
                    {
                        Poco::Logger::get("MediaProcessor").debug("Processing file in thread: " + file_path_copy);

                        // Mark file as in progress
                        ScannedFilesOps::markProcessed(*db_manager, file_path_copy, server_mode_copy, 1); // 1 = in progress

                        // Check if file exists and is accessible (file access error detection)
                        if (!std::filesystem::exists(file_path_copy))
                        {
                            Poco::Logger::get("MediaProcessor").error("File not found: " + file_path_copy);
                            ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, server_mode_copy, -3); // File access error
                            return;
                        }

                        if (!std::filesystem::is_regular_file(file_path_copy))
                        {
                            Poco::Logger::get("MediaProcessor").error("File is not a regular file: " + file_path_copy);
                            ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, server_mode_copy, -3); // File access error
                            return;
                        }

                        AudioProcessor audio_processor;
                        bool processing_success = false;

                        // Process based on server mode (media loading happens here on-demand)
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
                            ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, server_mode_copy, -1); // General processing error
                            Poco::Logger::get("MediaProcessor").warning("Failed to process file: " + file_path_copy);
                        }
                    }
                    catch (const std::bad_alloc& e)
                    {
                        Poco::Logger::get("MediaProcessor").error("Memory allocation error processing file " + file_path_copy + ": " + e.what());
                        ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, server_mode_copy, -4); // Memory allocation error
                    }
                    catch (const std::filesystem::filesystem_error& e)
                    {
                        Poco::Logger::get("MediaProcessor").error("File system error processing file " + file_path_copy + ": " + e.what());
                        ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, server_mode_copy, -3); // File access error
                    }
                    catch (const std::runtime_error& e)
                    {
                        std::string error_msg = e.what();
                        // Check for network-related error messages
                        if (error_msg.find("network") != std::string::npos || 
                            error_msg.find("connection") != std::string::npos ||
                            error_msg.find("timeout") != std::string::npos)
                        {
                            Poco::Logger::get("MediaProcessor").error("Network error processing file " + file_path_copy + ": " + error_msg);
                            ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, server_mode_copy, -5); // Network-related error
                        }
                        else
                        {
                            Poco::Logger::get("MediaProcessor").error("Runtime error processing file " + file_path_copy + ": " + error_msg);
                            ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, server_mode_copy, -1); // General error
                        }
                    }
                    catch (const std::exception& e)
                    {
                        Poco::Logger::get("MediaProcessor").error("Exception in audio processing thread for file " + file_path_copy + ": " + e.what());
                        ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, server_mode_copy, -1); // General error
                    }
                    catch (...)
                    {
                        Poco::Logger::get("MediaProcessor").error("Unknown exception in audio processing thread for file: " + file_path_copy);
                        ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, server_mode_copy, -1); // General error
                    } }, file_path_copy);

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
            // Query unprocessed files limited to current processing queue size to prevent memory buildup
            std::vector<ScannedFileRow> unprocessed_files = ScannedFilesOps::listUnprocessed(*database_manager_, current_mode, static_cast<int>(max_processing_queue_size_));

            Poco::Logger::get("MediaProcessor").information("Found " + std::to_string(unprocessed_files.size()) + " unprocessed files for current server mode (limited to " + std::to_string(max_processing_queue_size_) + " by queue size)");

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
                        // Check if file was already marked as skipped due to backpressure
                        // (RouteToProcessorInternal already marked it with status -2)
                        // Only mark as failed (-1) if it wasn't already processed
                        try
                        {
                            auto existing = ScannedFilesOps::getByPath(*database_manager_, file_row.file_path);
                            if (existing)
                            {
                                // Check the appropriate field based on current mode
                                int current_status = 0;
                                switch (current_mode)
                                {
                                case ServerMode::FAST:
                                    current_status = existing->processed_fast;
                                    break;
                                case ServerMode::BALANCED:
                                    current_status = existing->processed_balanced;
                                    break;
                                case ServerMode::QUALITY:
                                    current_status = existing->processed_quality;
                                    break;
                                }

                                if (current_status != -2) // Not already marked as skipped
                                {
                                    ScannedFilesOps::markProcessedWithEscalation(*database_manager_, file_row.file_path, current_mode, -1);
                                    error_count++;
                                    Poco::Logger::get("MediaProcessor").warning("Failed to submit file for processing: " + file_row.file_path);
                                }
                                else
                                {
                                    // File was skipped due to backpressure, don't double-mark as error
                                    Poco::Logger::get("MediaProcessor").debug("File skipped due to backpressure: " + file_row.file_path);
                                }
                            }
                            else
                            {
                                // No existing record, mark as failed
                                ScannedFilesOps::markProcessedWithEscalation(*database_manager_, file_row.file_path, current_mode, -1);
                                error_count++;
                                Poco::Logger::get("MediaProcessor").warning("Failed to submit file for processing: " + file_row.file_path);
                            }
                        }
                        catch (...)
                        {
                            // Fallback: mark as failed if we can't check status
                            ScannedFilesOps::markProcessedWithEscalation(*database_manager_, file_row.file_path, current_mode, -1);
                            error_count++;
                            Poco::Logger::get("MediaProcessor").warning("Failed to submit file for processing (fallback): " + file_row.file_path);
                        }
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
        // React to unified thread pool configuration changes
        if (event.key == "media.processor.threadPool.share.media_processor")
        {
            if (thread_pool_manager_)
            {
                double new_share = config_manager_->getPropertyValue<double>(event.key, 1.0);
                thread_pool_manager_->setShare("media_processor", new_share);
                Poco::Logger::get("MediaProcessor").information("Updated thread pool share for media_processor: " + std::to_string(new_share));
            }
            return;
        }

        // React to unified queue size configuration changes
        if (event.key == "media.processor.maxQueueSize")
        {
            max_processing_queue_size_ = static_cast<size_t>(config_manager_->getPropertyValue<int>(event.key, 10000));
            Poco::Logger::get("MediaProcessor").information("Updated unified processing queue size limit: %u", static_cast<unsigned int>(max_processing_queue_size_));
            return;
        }

        // React to transcoding configuration changes
        if (event.key == "media.image.transcoding.enabled" ||
            event.key == "media.image.transcoding.timeoutMs" ||
            event.key == "media.image.transcoding.preserveMetadata")
        {
            Poco::Logger::get("MediaProcessor").information("Transcoding configuration changed: %s", event.key);

            // Log the change details for debugging
            try
            {
                if (event.key == "media.image.transcoding.enabled")
                {
                    bool new_value = config_manager_->getPropertyValue<bool>(event.key, true);
                    Poco::Logger::get("MediaProcessor").information("Transcoding enabled: %s", new_value ? "true" : "false");
                }
                else if (event.key == "media.image.transcoding.timeoutMs")
                {
                    int new_value = config_manager_->getPropertyValue<int>(event.key, 60000);
                    Poco::Logger::get("MediaProcessor").information("Transcoding timeout: %d ms", new_value);
                }
                else if (event.key == "media.image.transcoding.preserveMetadata")
                {
                    bool new_value = config_manager_->getPropertyValue<bool>(event.key, true);
                    Poco::Logger::get("MediaProcessor").information("Transcoding preserve metadata: %s", new_value ? "true" : "false");
                }
            }
            catch (const std::exception &e)
            {
                Poco::Logger::get("MediaProcessor").warning("Failed to log transcoding config change for %s: %s", event.key, e.what());
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
