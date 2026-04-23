#include "media_processors/media_processor.hpp"
#include "media_processors/image_processor.hpp"
#include "media_processors/image/pipeline_thumbnail_helper.hpp"
#include "media_processors/audio_processor.hpp"
#include "media_processors/video_processor.hpp"
#include "media_processors/image/backends/raw_file_detector.hpp"
#include "media_processors/image/backends/transcoding_pipeline.hpp"
#include "filesmanager/disk_cache.hpp"
#include "config/config_enums.hpp"
#include "database/database_manager.hpp"
#include "database/scanned_files_ops.hpp"
#include "database/processing_errors_ops.hpp"
#include "database/preview_cache_ops.hpp"
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
                                   std::shared_ptr<ThreadPoolManager> thread_pool_manager,
                                   std::shared_ptr<DiskCache> thumbnail_cache)
        : config_manager_(config_manager), database_manager_(database_manager), thread_pool_manager_(thread_pool_manager), thumbnail_cache_(thumbnail_cache)
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

        // Set up unified thread pool shares for media processing (consolidated naming: tpm.types.<type>.share)
        double media_processor_share = config_manager_->getPropertyValue<double>("tpm.types.media_processor.share", 1.0);
        thread_pool_manager_->setShare("media_processor", media_processor_share);

        // Initialize reserved thread type shares (for future audio/video/image processors)
        double image_processor_share = config_manager_->getPropertyValue<double>("tpm.types.image_processor.share", 1.0);
        thread_pool_manager_->setShare("image_processor", image_processor_share);

        double audio_processor_share = config_manager_->getPropertyValue<double>("tpm.types.audio_processor.share", 1.0);
        thread_pool_manager_->setShare("audio_processor", audio_processor_share);

        double video_processor_share = config_manager_->getPropertyValue<double>("tpm.types.video_processor.share", 1.0);
        thread_pool_manager_->setShare("video_processor", video_processor_share);

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

        // Single processing mode (embedding-based)

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
                        ScannedFilesOps::markProcessed(*database_manager_, file_path, -2); // -2 = skipped due to backpressure
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
                    ScannedFilesOps::markProcessed(*database_manager_, file_path, -99); // -99 = queued
                }
                catch (...)
                {
                    Poco::Logger::get("MediaProcessor").error("Failed to mark file as queued in database: %s", file_path);
                }

                // Capture by value for thread safety and to avoid dangling references
                std::string file_path_copy = file_path;
                std::shared_ptr<DatabaseManager> db_manager = database_manager_;
                std::shared_ptr<UnifiedObservableConfigManager> config_manager = config_manager_;
                std::shared_ptr<DiskCache> disk_cache = disk_cache_;
                std::shared_ptr<DiskCache> thumbnail_cache = thumbnail_cache_;

                thread_pool_manager_->submit("media_processor", [file_path_copy, db_manager, config_manager, disk_cache, thumbnail_cache]()
                                             {
                    try
                    {
                        Poco::Logger::get("MediaProcessor").debug("Processing file in thread: " + file_path_copy);

                        // Check if file is already processed or currently being processed
                        auto current_file = ScannedFilesOps::getByPath(*db_manager, file_path_copy);
                        if (current_file.has_value())
                        {
                            int current_status = current_file->processed;

                            // Skip if already processed (status >= 2) or currently in progress (status = 1)
                            if (current_status >= 2)
                            {
                                Poco::Logger::get("MediaProcessor").information("Skipping already processed file: %s (status: %d)", file_path_copy, current_status);
                                return;
                            }
                            else if (current_status == 1)
                            {
                                Poco::Logger::get("MediaProcessor").information("Skipping file currently in progress: %s", file_path_copy);
                                return;
                        }
                    }

                    // Mark file as in progress (only if not already in an error state)
                    // If file is in an error state, keep it so escalation logic can work properly
                    if (current_file.has_value())
                    {
                        int current_status = current_file->processed;
                        
                        // Only mark as "in progress" if not already in an error state
                        if (current_status >= 0)
                        {
                            ScannedFilesOps::markProcessed(*db_manager, file_path_copy, 1); // 1 = in progress
                        }
                    }

                    // We no longer copy files to the cache before processing!
                    std::string processing_file_path = file_path_copy;
                    std::string transcoded_preview_path; // Track if we created a permanent preview
                    bool is_temporary_transcode = false; // Track if we need to delete processing_file_path

                    // Check if file needs transcoding
                    bool needs_transcoding = RawFileDetector::IsRawFile(file_path_copy);
                    
                    if (needs_transcoding)
                    {
                        Poco::Logger::get("MediaProcessor").information("Raw file detected, checking preview cache: %s", file_path_copy);
                        
                        // Helper to get mtime safely
                        int64_t source_mtime = 0;
                        try {
                            auto ftime = std::filesystem::last_write_time(file_path_copy);
                            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                                ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
                            source_mtime = std::chrono::system_clock::to_time_t(sctp);
                        } catch (...) {}

                        auto existing_preview = PreviewCacheOps::getPreview(*db_manager, file_path_copy);

                        if (existing_preview && existing_preview->source_modified_at == source_mtime && std::filesystem::exists(existing_preview->cached_path))
                        {
                            Poco::Logger::get("MediaProcessor").information("Using existing preview cache for RAW file: %s", existing_preview->cached_path);
                            processing_file_path = existing_preview->cached_path;
                            transcoded_preview_path = existing_preview->cached_path;
                        }
                        else
                        {
                            // Transcode to a new file in cache
                            std::string transcoded_filename;
                            TranscodingConfig transcode_config = TranscodingPipeline::GetConfigFromManager(config_manager);
                            
                            std::string cache_directory = disk_cache->getCacheLocation();
                            bool transcode_success = TranscodingPipeline::TranscodeToFile(file_path_copy, transcoded_filename, transcode_config, cache_directory);
                            
                            if (transcode_success && std::filesystem::exists(transcoded_filename) && std::filesystem::file_size(transcoded_filename) > 0)
                            {
                                // Save to preview cache
                                PreviewCacheRecord preview_record;
                                preview_record.source_path = file_path_copy;
                                preview_record.cached_path = transcoded_filename;
                                preview_record.file_size_bytes = std::filesystem::file_size(transcoded_filename);
                                preview_record.source_modified_at = source_mtime;
                                int64_t current_time = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                                preview_record.created_at = current_time;
                                preview_record.last_accessed_at = current_time;
                                
                                PreviewCacheOps::upsertPreview(*db_manager, preview_record);
                                
                                processing_file_path = transcoded_filename;
                                transcoded_preview_path = transcoded_filename;
                                Poco::Logger::get("MediaProcessor").information("Successfully transcoded RAW and saved to preview cache: %s", transcoded_filename);
                            }
                            else
                            {
                                Poco::Logger::get("MediaProcessor").error("Failed to transcode raw file: %s", file_path_copy);
                                ProcessingErrorsOps::insertError(*db_manager, file_path_copy, ServerMode::EMBEDDING, -1, "Failed to transcode raw file (unsupported or corrupted)", "ImageMagick");
                                ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, -1);
                                if (std::filesystem::exists(transcoded_filename)) std::filesystem::remove(transcoded_filename);
                                return;
                            }
                        }
                    }
                    else if (TranscodingPipeline::NeedsTranscodingForEmbedding(file_path_copy, config_manager))
                    {
                        // Non-RAW formats (e.g. BMP) that OpenCV may not read reliably:
                        // transcode to JPEG via ImageMagick so the embedding pipeline gets a format it can read
                        std::string transcoded_filename;
                        std::string cache_directory = disk_cache->getCacheLocation();
                        if (TranscodingPipeline::TranscodeNonRawToFile(file_path_copy, transcoded_filename, cache_directory))
                        {
                            processing_file_path = transcoded_filename;
                            is_temporary_transcode = true;
                            disk_cache->markFileInUse(processing_file_path);
                            Poco::Logger::get("MediaProcessor").information("Transcoded image for embedding: %s -> %s", file_path_copy, transcoded_filename);
                        }
                        else
                        {
                            Poco::Logger::get("MediaProcessor").error("Failed to transcode image for embedding: %s", file_path_copy);
                            ProcessingErrorsOps::insertError(*db_manager, file_path_copy, ServerMode::EMBEDDING, -1, "Failed to transcode image to JPEG for embedding", "ImageMagick");
                            ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, -1);
                            return;
                        }
                    }

                    Poco::Logger::get("MediaProcessor").trace("About to process file - processing_path: %s, original_path: %s", processing_file_path, file_path_copy);
                    
                    try
                    {
                        ImageProcessor image_processor;
                        bool processing_success = false;

                        Poco::Logger::get("MediaProcessor").trace("Calling image_processor.Process with processing_path: %s", processing_file_path);
                        
                        // Process image
                        processing_success = image_processor.Process(processing_file_path, file_path_copy, *db_manager, config_manager);
                        
                        Poco::Logger::get("MediaProcessor").trace("image_processor.Process returned: %s", processing_success ? "true" : "false");

                        // Update database status within the lambda using connection pool
                        if (processing_success)
                        {
                            ScannedFilesOps::markProcessed(*db_manager, file_path_copy, 2);
                            Poco::Logger::get("MediaProcessor").debug("Successfully completed processing file: " + file_path_copy);

                            // Generate thumbnail after successful processing (non-blocking)
                            // Provide transcoded_preview_path so ThumbnailGenerator doesn't decode RAW again
                            if (thumbnail_cache)
                            {
                                bool thumb_success = PipelineThumbnailHelper::generateThumbnail(
                                    file_path_copy, *db_manager, *thumbnail_cache, *config_manager, transcoded_preview_path);
                                if (!thumb_success)
                                {
                                    Poco::Logger::get("MediaProcessor").debug(
                                        "Pipeline thumbnail generation failed (non-critical): " + file_path_copy);
                                }
                            }
                        }
                        else
                        {
                            // Pipeline already logged specific error, just mark as failed
                            ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, -1); // General processing error
                            Poco::Logger::get("MediaProcessor").warning("Failed to process file: " + file_path_copy);
                        }
                    }
                    catch (...)
                    {
                        throw;
                    }

                    // Cleanup: Delete temporary transcodes (but NOT permanent preview_cache items or original files)
                    if (is_temporary_transcode)
                    {
                        disk_cache->markFileNotInUse(processing_file_path);
                        disk_cache->deleteFromCacheImmediately(processing_file_path);
                    }
                    }
                    catch (const std::bad_alloc& e)
                    {
                        Poco::Logger::get("MediaProcessor").error("Memory allocation error processing file " + file_path_copy + ": " + e.what());
                        ProcessingErrorsOps::insertError(*db_manager, file_path_copy, ServerMode::EMBEDDING, -4, e.what(), "Memory");
                        ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, -4); // Memory allocation error
                    }
                    catch (const std::filesystem::filesystem_error& e)
                    {
                        Poco::Logger::get("MediaProcessor").error("File system error processing file " + file_path_copy + ": " + e.what());
                        ProcessingErrorsOps::insertError(*db_manager, file_path_copy, ServerMode::EMBEDDING, -3, e.what(), "FileSystem");
                        ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, -3); // File access error
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
                            ProcessingErrorsOps::insertError(*db_manager, file_path_copy, ServerMode::EMBEDDING, -5, error_msg, "Network");
                            ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, -5); // Network-related error
                        }
                        else
                        {
                            Poco::Logger::get("MediaProcessor").error("Runtime error processing file " + file_path_copy + ": " + error_msg);
                            ProcessingErrorsOps::insertError(*db_manager, file_path_copy, ServerMode::EMBEDDING, -1, error_msg, "General");
                            ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, -1); // General error
                        }
                    }
                    catch (const std::exception& e)
                    {
                        Poco::Logger::get("MediaProcessor").error("Exception in image processing thread for file " + file_path_copy + ": " + e.what());
                        ProcessingErrorsOps::insertError(*db_manager, file_path_copy, ServerMode::EMBEDDING, -1, e.what(), "General");
                        ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, -1); // General error
                    }
                    catch (...)
                    {
                        Poco::Logger::get("MediaProcessor").error("Unknown exception in image processing thread for file: " + file_path_copy);
                        ProcessingErrorsOps::insertError(*db_manager, file_path_copy, ServerMode::EMBEDDING, -1, "Unknown exception", "General");
                        ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, -1); // General error
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
                        ScannedFilesOps::markProcessed(*database_manager_, file_path, -2); // -2 = skipped due to backpressure
                    }
                    catch (...)
                    {
                        Poco::Logger::get("MediaProcessor").error("Failed to mark file as skipped in database: %s", file_path);
                    }
                    return false; // Indicate that processing was skipped
                }

                // Capture by value for thread safety and to avoid dangling references
                std::string file_path_copy = file_path;
                std::shared_ptr<DatabaseManager> db_manager = database_manager_;

                thread_pool_manager_->submit("media_processor", [file_path_copy, db_manager]()
                                             {
                    try
                    {
                        Poco::Logger::get("MediaProcessor").debug("Processing file in thread: " + file_path_copy);

                        // Check if file is already processed or currently being processed
                        auto current_file = ScannedFilesOps::getByPath(*db_manager, file_path_copy);
                        if (current_file.has_value())
                        {
                            int current_status = current_file->processed;

                            // Skip if already processed (status >= 2) or currently in progress (status = 1)
                            if (current_status >= 2)
                            {
                                Poco::Logger::get("MediaProcessor").information("Skipping already processed file: %s (status: %d)", file_path_copy, current_status);
                                return;
                            }
                            else if (current_status == 1)
                            {
                                Poco::Logger::get("MediaProcessor").information("Skipping file currently in progress: %s", file_path_copy);
                                return;
                            }
                        }

                        // Mark file as in progress (only if not already in an error state)
                        // If file is in an error state, keep it so escalation logic can work properly
                        if (current_file.has_value())
                        {
                            int current_status_check = current_file->processed;
                            
                            // Only mark as "in progress" if not already in an error state
                            if (current_status_check >= 0)
                            {
                                ScannedFilesOps::markProcessed(*db_manager, file_path_copy, 1); // 1 = in progress
                            }
                        }

                        // Check if file exists and is accessible (file access error detection)
                        if (!std::filesystem::exists(file_path_copy))
                        {
                            Poco::Logger::get("MediaProcessor").error("File not found: " + file_path_copy);
                            ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, -3); // File access error
                            return;
                        }

                        if (!std::filesystem::is_regular_file(file_path_copy))
                        {
                            Poco::Logger::get("MediaProcessor").error("File is not a regular file: " + file_path_copy);
                            ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, -3); // File access error
                            return;
                        }

                        VideoProcessor video_processor;
                        bool processing_success = false;

                        // Process video (media loading happens here on-demand)
                        processing_success = video_processor.Process(file_path_copy);

                        // Update database status within the lambda using connection pool
                        if (processing_success)
                        {
                            ScannedFilesOps::markProcessed(*db_manager, file_path_copy, 2);
                            Poco::Logger::get("MediaProcessor").debug("Successfully completed processing file: " + file_path_copy);
                        }
                        else
                        {
                            ProcessingErrorsOps::insertError(*db_manager, file_path_copy, ServerMode::EMBEDDING, -1, "Video processing failed", "Video");
                            ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, -1); // General processing error
                            Poco::Logger::get("MediaProcessor").warning("Failed to process file: " + file_path_copy);
                        }
                    }
                    catch (const std::bad_alloc& e)
                    {
                        Poco::Logger::get("MediaProcessor").error("Memory allocation error processing file " + file_path_copy + ": " + e.what());
                        ProcessingErrorsOps::insertError(*db_manager, file_path_copy, ServerMode::EMBEDDING, -4, e.what(), "Memory");
                        ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, -4); // Memory allocation error
                    }
                    catch (const std::filesystem::filesystem_error& e)
                    {
                        Poco::Logger::get("MediaProcessor").error("File system error processing file " + file_path_copy + ": " + e.what());
                        ProcessingErrorsOps::insertError(*db_manager, file_path_copy, ServerMode::EMBEDDING, -3, e.what(), "FileSystem");
                        ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, -3); // File access error
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
                            ProcessingErrorsOps::insertError(*db_manager, file_path_copy, ServerMode::EMBEDDING, -5, error_msg, "Network");
                            ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, -5); // Network-related error
                        }
                        else
                        {
                            Poco::Logger::get("MediaProcessor").error("Runtime error processing file " + file_path_copy + ": " + error_msg);
                            ProcessingErrorsOps::insertError(*db_manager, file_path_copy, ServerMode::EMBEDDING, -1, error_msg, "General");
                            ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, -1); // General error
                        }
                    }
                    catch (const std::exception& e)
                    {
                        Poco::Logger::get("MediaProcessor").error("Exception in video processing thread for file " + file_path_copy + ": " + e.what());
                        ProcessingErrorsOps::insertError(*db_manager, file_path_copy, ServerMode::EMBEDDING, -1, e.what(), "General");
                        ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, -1); // General error
                    }
                    catch (...)
                    {
                        Poco::Logger::get("MediaProcessor").error("Unknown exception in video processing thread for file: " + file_path_copy);
                        ProcessingErrorsOps::insertError(*db_manager, file_path_copy, ServerMode::EMBEDDING, -1, "Unknown exception", "General");
                        ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, -1); // General error
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
                        ScannedFilesOps::markProcessed(*database_manager_, file_path, -2); // -2 = skipped due to backpressure
                    }
                    catch (...)
                    {
                        Poco::Logger::get("MediaProcessor").error("Failed to mark file as skipped in database: %s", file_path);
                    }
                    return false; // Indicate that processing was skipped
                }

                // Capture by value for thread safety and to avoid dangling references
                std::string file_path_copy = file_path;
                std::shared_ptr<DatabaseManager> db_manager = database_manager_;

                thread_pool_manager_->submit("media_processor", [file_path_copy, db_manager]()
                                             {
                    try
                    {
                        Poco::Logger::get("MediaProcessor").debug("Processing file in thread: " + file_path_copy);

                        // Check if file is already processed or currently being processed
                        auto current_file = ScannedFilesOps::getByPath(*db_manager, file_path_copy);
                        if (current_file.has_value())
                        {
                            int current_status = current_file->processed;

                            // Skip if already processed (status >= 2) or currently in progress (status = 1)
                            if (current_status >= 2)
                            {
                                Poco::Logger::get("MediaProcessor").information("Skipping already processed file: %s (status: %d)", file_path_copy, current_status);
                                return;
                            }
                            else if (current_status == 1)
                            {
                                Poco::Logger::get("MediaProcessor").information("Skipping file currently in progress: %s", file_path_copy);
                                return;
                            }
                        }

                        // Mark file as in progress (only if not already in an error state)
                        // If file is in an error state, keep it so escalation logic can work properly
                        if (current_file.has_value())
                        {
                            int current_status_check = current_file->processed;
                            
                            // Only mark as "in progress" if not already in an error state
                            if (current_status_check >= 0)
                            {
                                ScannedFilesOps::markProcessed(*db_manager, file_path_copy, 1); // 1 = in progress
                            }
                        }

                        // Check if file exists and is accessible (file access error detection)
                        if (!std::filesystem::exists(file_path_copy))
                        {
                            Poco::Logger::get("MediaProcessor").error("File not found: " + file_path_copy);
                            ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, -3); // File access error
                            return;
                        }

                        if (!std::filesystem::is_regular_file(file_path_copy))
                        {
                            Poco::Logger::get("MediaProcessor").error("File is not a regular file: " + file_path_copy);
                            ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, -3); // File access error
                            return;
                        }

                        AudioProcessor audio_processor;
                        bool processing_success = false;

                        // Process audio (media loading happens here on-demand)
                        processing_success = audio_processor.Process(file_path_copy);

                        // Update database status within the lambda using connection pool
                        if (processing_success)
                        {
                            ScannedFilesOps::markProcessed(*db_manager, file_path_copy, 2);
                            Poco::Logger::get("MediaProcessor").debug("Successfully completed processing file: " + file_path_copy);
                        }
                        else
                        {
                            ProcessingErrorsOps::insertError(*db_manager, file_path_copy, ServerMode::EMBEDDING, -1, "Audio processing failed", "Audio");
                            ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, -1); // General processing error
                            Poco::Logger::get("MediaProcessor").warning("Failed to process file: " + file_path_copy);
                        }
                    }
                    catch (const std::bad_alloc& e)
                    {
                        Poco::Logger::get("MediaProcessor").error("Memory allocation error processing file " + file_path_copy + ": " + e.what());
                        ProcessingErrorsOps::insertError(*db_manager, file_path_copy, ServerMode::EMBEDDING, -4, e.what(), "Memory");
                        ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, -4); // Memory allocation error
                    }
                    catch (const std::filesystem::filesystem_error& e)
                    {
                        Poco::Logger::get("MediaProcessor").error("File system error processing file " + file_path_copy + ": " + e.what());
                        ProcessingErrorsOps::insertError(*db_manager, file_path_copy, ServerMode::EMBEDDING, -3, e.what(), "FileSystem");
                        ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, -3); // File access error
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
                            ProcessingErrorsOps::insertError(*db_manager, file_path_copy, ServerMode::EMBEDDING, -5, error_msg, "Network");
                            ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, -5); // Network-related error
                        }
                        else
                        {
                            Poco::Logger::get("MediaProcessor").error("Runtime error processing file " + file_path_copy + ": " + error_msg);
                            ProcessingErrorsOps::insertError(*db_manager, file_path_copy, ServerMode::EMBEDDING, -1, error_msg, "General");
                            ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, -1); // General error
                        }
                    }
                    catch (const std::exception& e)
                    {
                        Poco::Logger::get("MediaProcessor").error("Exception in audio processing thread for file " + file_path_copy + ": " + e.what());
                        ProcessingErrorsOps::insertError(*db_manager, file_path_copy, ServerMode::EMBEDDING, -1, e.what(), "General");
                        ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, -1); // General error
                    }
                    catch (...)
                    {
                        Poco::Logger::get("MediaProcessor").error("Unknown exception in audio processing thread for file: " + file_path_copy);
                        ProcessingErrorsOps::insertError(*db_manager, file_path_copy, ServerMode::EMBEDDING, -1, "Unknown exception", "General");
                        ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, -1); // General error
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

        Poco::Logger::get("MediaProcessor").information("Processing media files");

        try
        {
            // Check available queue space before fetching files to prevent scheduler thrashing
            size_t current_queue_depth = thread_pool_manager_->getQueueDepth("media_processor");
            size_t available_space = (max_processing_queue_size_ > current_queue_depth)
                                         ? (max_processing_queue_size_ - current_queue_depth)
                                         : 0;

            if (available_space == 0)
            {
                Poco::Logger::get("MediaProcessor").debug("Media processor queue is full (%zu/%zu), skipping this cycle to avoid thrashing", current_queue_depth, max_processing_queue_size_);
                return;
            }

            Poco::Logger::get("MediaProcessor").debug("Media processor queue has %zu available slots (current: %zu, max: %zu)", available_space, current_queue_depth, max_processing_queue_size_);

            // Query unprocessed files limited to available queue space to prevent memory buildup and backpressure thrashing
            std::vector<ScannedFileRow> unprocessed_files = ScannedFilesOps::listUnprocessed(*database_manager_, static_cast<int>(available_space));

            Poco::Logger::get("MediaProcessor").information("Found " + std::to_string(unprocessed_files.size()) + " unprocessed files for current server mode (limited to " + std::to_string(available_space) + " by available queue space)");

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
                                // Check the current status
                                int current_status = existing->processed;

                                if (current_status != -2) // Not already marked as skipped
                                {
                                    ScannedFilesOps::markProcessedWithEscalation(*database_manager_, file_row.file_path, -1);
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
                                ScannedFilesOps::markProcessedWithEscalation(*database_manager_, file_row.file_path, -1);
                                error_count++;
                                Poco::Logger::get("MediaProcessor").warning("Failed to submit file for processing: " + file_row.file_path);
                            }
                        }
                        catch (...)
                        {
                            // Fallback: mark as failed if we can't check status
                            ScannedFilesOps::markProcessedWithEscalation(*database_manager_, file_row.file_path, -1);
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
        // React to unified thread pool configuration changes (consolidated naming: tpm.types.<type>.share)
        if (event.key == "tpm.types.media_processor.share")
        {
            if (thread_pool_manager_)
            {
                double new_share = config_manager_->getPropertyValue<double>(event.key, 1.0);
                thread_pool_manager_->setShare("media_processor", new_share);
                Poco::Logger::get("MediaProcessor").information("Updated thread pool share for media_processor: " + std::to_string(new_share));
            }
            return;
        }

        // React to reserved thread type share changes (for future use)
        if (event.key == "tpm.types.image_processor.share" ||
            event.key == "tpm.types.audio_processor.share" ||
            event.key == "tpm.types.video_processor.share")
        {
            if (thread_pool_manager_)
            {
                // Extract type name from key (e.g., "tpm.types.image_processor.share" -> "image_processor")
                std::string type_name = event.key.substr(10); // skip "tpm.types."
                type_name = type_name.substr(0, type_name.find(".share"));

                double new_share = config_manager_->getPropertyValue<double>(event.key, 1.0);
                thread_pool_manager_->setShare(type_name, new_share);
                Poco::Logger::get("MediaProcessor").information("Updated thread pool share for %s (reserved): %f", type_name, new_share);
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
        return config_manager_->getServerMode("server.mode", ServerMode::EMBEDDING);
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
