#include "core/media_processing_orchestrator.hpp"
#include "core/media_processor.hpp"
#include "core/transcoding_manager.hpp"
#include "database/database_manager.hpp"
#include "core/duplicate_linker.hpp"
#include "logging/logger.hpp"
#include <chrono>
#include <thread>
#include <condition_variable>
#include <tbb/parallel_for.h>
#include <tbb/blocked_range.h>
#include <tbb/mutex.h>
#include <filesystem>
#include <iostream> // Added for stdout logging

MediaProcessingOrchestrator::MediaProcessingOrchestrator(DatabaseManager &dbMan)
    : dbMan_(dbMan), cancelled_(false)
{
    // Subscribe to configuration changes
    ServerConfigManager::getInstance().subscribe(this);
}

MediaProcessingOrchestrator::~MediaProcessingOrchestrator()
{
    // Ensure cancellation is set to prevent any ongoing operations
    cancelled_.store(true);

    // Stop timer-based processing if running
    stopTimerBasedProcessing();

    // Unsubscribe from configuration changes
    ServerConfigManager::getInstance().unsubscribe(this);

    Logger::debug("MediaProcessingOrchestrator destructor called");
}

bool MediaProcessingOrchestrator::tryAcquireProcessingLock(const std::string &file_path)
{
    std::unique_lock<std::shared_mutex> lock(processing_state_mutex_);

    // Check if file is already being processed
    if (currently_processing_files_.find(file_path) != currently_processing_files_.end())
    {
        Logger::debug("File already being processed, skipping: " + file_path);
        return false;
    }

    // Acquire the lock by adding to the set
    currently_processing_files_.insert(file_path);
    Logger::debug("Acquired processing lock for: " + file_path);
    return true;
}

void MediaProcessingOrchestrator::releaseProcessingLock(const std::string &file_path)
{
    std::unique_lock<std::shared_mutex> lock(processing_state_mutex_);

    // Release the lock by removing from the set
    auto it = currently_processing_files_.find(file_path);
    if (it != currently_processing_files_.end())
    {
        currently_processing_files_.erase(it);
        Logger::debug("Released processing lock for: " + file_path);
    }
}

SimpleObservable<FileProcessingEvent> MediaProcessingOrchestrator::processAllScannedFiles(int max_threads)
{
    // Use a mutex to ensure only one processing run happens at a time
    std::unique_lock<std::mutex> processing_run_lock(processing_mutex_);

    // Error handling policy:
    // - All errors are logged with context
    // - Per-file errors are emitted via onNext (with success=false)
    // - Fatal errors (DB not available, invalid config, cancellation) are emitted via onError
    // - No silent failures
    // Use configuration if max_threads is -1 (default)
    int actual_max_threads = max_threads;
    if (actual_max_threads == -1)
    {
        auto &config_manager = ServerConfigManager::getInstance();
        actual_max_threads = config_manager.getMaxProcessingThreads();
    }

    return SimpleObservable<FileProcessingEvent>([this, actual_max_threads](auto onNext, auto onError, auto onComplete)
                                                 {
        try {
            cancelled_ = false;
            if (!dbMan_.isValid()) {
                Logger::error("Database not initialized or invalid");
                if (onError) onError(std::runtime_error("Database not initialized"));
                return;
            }
            auto& config_manager = ServerConfigManager::getInstance();
            DedupMode mode = config_manager.getDedupMode();
            bool pre_process_quality_stack = config_manager.getPreProcessQualityStack();
            
            if (mode != DedupMode::FAST && mode != DedupMode::BALANCED && mode != DedupMode::QUALITY) {
                Logger::error("Invalid dedup mode configured");
                if (onError) onError(std::runtime_error("Invalid dedup mode"));
                return;
            }
            
            // Determine which modes to process
            std::vector<DedupMode> modes_to_process;
            if (pre_process_quality_stack) {
                // Process all three quality levels
                modes_to_process = {DedupMode::FAST, DedupMode::BALANCED, DedupMode::QUALITY};
                Logger::info("PreProcessQualityStack enabled - processing all quality levels (FAST, BALANCED, QUALITY)");
            } else {
                // Process only the selected mode
                modes_to_process = {mode};
                Logger::info("PreProcessQualityStack disabled - processing only selected mode: " + DedupModes::getModeName(mode));
            }
            
            // Get files that need processing for any of the modes
            std::vector<std::pair<std::string, std::string>> files_to_process;
            
            // Get configurable batch size
            int batch_size = config_manager.getProcessingBatchSize();
            
            if (pre_process_quality_stack) {
                // Get files that need processing for ANY mode and atomically mark them as in progress
                // This prevents race conditions where multiple threads get the same files
                files_to_process = dbMan_.getAndMarkFilesForProcessingAnyModeWithPriority(batch_size);
            } else {
                // Get files that need processing for the specific mode using atomic batch processing
                files_to_process = dbMan_.getAndMarkFilesForProcessingWithPriority(mode, batch_size);
            }
            
            if (files_to_process.empty()) {
                Logger::info("No files need processing");
                if (onComplete) onComplete();
                return;
            }
            
            Logger::info("Starting processing of " + std::to_string(files_to_process.size()) + 
                        " files with " + std::to_string(actual_max_threads) + " threads");
            
            // Thread-safe counters for progress tracking
            std::atomic<size_t> processed_count{0};
            std::atomic<size_t> successful_processed{0};
            std::atomic<size_t> failed_processed{0};
            
            // Thread-safe mutex for database operations to prevent race conditions
            tbb::mutex db_mutex;
            
            // Process files sequentially to avoid race conditions when processing multiple modes per file
            // When pre_process_quality_stack is enabled, each file needs to be processed for all modes
            // Processing in parallel can cause race conditions and duplicate processing
            for (size_t i = 0; i < files_to_process.size(); ++i) {
                // Check for cancellation
                if (cancelled_.load()) {
                    return; // Exit this thread's processing
                }
                
                const std::string& file_path = files_to_process[i].first;
                        
                        auto start = std::chrono::steady_clock::now();
                        FileProcessingEvent event;
                        event.file_path = file_path;
                        
                        try {
                            // Check if file is supported
                            if (!MediaProcessor::isSupportedFile(file_path)) {
                                event.success = false;
                                event.error_message = "Unsupported file type: " + file_path;
                                Logger::error(event.error_message);
                                processed_count.fetch_add(1);
                                failed_processed.fetch_add(1);
                                if (onNext) onNext(event);
                                continue;
                            }
                            
                            // Process the file for each required mode
                            // Files are already marked as in progress (-1) by getAndMarkFilesForProcessing
                            bool any_success = false;
                            std::string last_error;
                            
                            for (const auto& process_mode : modes_to_process) {
                                // Files are already marked as in progress by getAndMarkFilesForProcessing
                                // No need to call tryAcquireProcessingLock again
                                Logger::info("Processing file: " + file_path + " with mode: " + DedupModes::getModeName(process_mode));
                                
                                // Check if this file has a transcoded version available
                                std::string actual_file_path = file_path;
                                std::string transcoded_path = TranscodingManager::getInstance().getTranscodedFilePath(file_path);
                                if (!transcoded_path.empty() && std::filesystem::exists(transcoded_path))
                                {
                                    actual_file_path = transcoded_path;
                                    Logger::debug("Using transcoded file for processing: " + file_path + " -> " + transcoded_path);
                                }
                                else if (TranscodingManager::isRawFile(file_path))
                                {
                                    // Raw file without a transcoded version yet – queue and defer processing
                                    Logger::info("Raw file missing transcoded output; queued and deferred: " + file_path);
                                    TranscodingManager::getInstance().queueForTranscoding(file_path);
                                    last_error = "Transcoding pending";
                                    // CRITICAL: Keep flag at -1 (in progress) - DO NOT change it!
                                    // The transcoding thread will reset it to 0 when transcoding completes
                                    Logger::debug("Keeping processing flag at -1 (in progress) for RAW file: " + file_path);
                                    failed_processed.fetch_add(1);
                                    continue;
                                }
                                
                                // Process the file for this mode
                                ProcessingResult result = MediaProcessor::processFile(actual_file_path, process_mode);
                                
                                // Store the processing result in the database
                                DBOpResult db_result = dbMan_.storeProcessingResult(file_path, process_mode, result);
                                if (!db_result.success)
                                {
                                    Logger::error("Failed to store processing result for: " + file_path + " - " + db_result.error_message);
                                    last_error = "Database error: " + db_result.error_message;
                                    failed_processed.fetch_add(1);
                                    continue;
                                }
                                
                                // Store the processing result
                                if (result.success)
                                {
                                    Logger::info("Successfully processed file: " + file_path + " (format: " + result.artifact.format + ", confidence: " + std::to_string(result.artifact.confidence) + ")");
                                    any_success = true;
                                    
                                    // Mark as successfully processed
                                    dbMan_.setProcessingFlag(file_path, process_mode);
                                    
                                    // Update success counter
                                    successful_processed.fetch_add(1);

                                    // Notify duplicate linker that new results are available
                                    DuplicateLinker::getInstance().notifyNewResults();
                                }
                                else
                                {
                                    Logger::warn("Failed to process file: " + file_path + " - " + result.error_message);
                                    last_error = result.error_message;
                                    
                                    // Mark as failed (set to 2 for error state)
                                    dbMan_.setProcessingFlagError(file_path, process_mode);
                                    
                                    // Update failure counter
                                    failed_processed.fetch_add(1);
                                }
                                
                                // Update progress counter
                                processed_count.fetch_add(1);
                                
                                // Check for cancellation
                                if (cancelled_.load())
                                {
                                    Logger::info("Processing cancelled");
                                    return;
                                }
                            }
                            
                            // Set final event result
                            if (any_success) {
                                event.success = true;
                                successful_processed.fetch_add(1);
                            } else {
                                event.success = false;
                                event.error_message = "Processing failed for all modes: " + last_error;
                                failed_processed.fetch_add(1);
                            }
                            
                            // Success - update counters and emit event
                            auto end = std::chrono::steady_clock::now();
                            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
                            event.processing_time_ms = duration.count();
                            
                            processed_count.fetch_add(1);
                            
                            Logger::info("Successfully processed file: " + file_path + 
                                       " (format: " + event.artifact_format + 
                                       ", confidence: " + std::to_string(event.artifact_confidence) + 
                                       ", time: " + std::to_string(event.processing_time_ms) + "ms)");
                            
                            if (onNext) onNext(event);
                            
                        } catch (const std::exception& e) {
                            Logger::error("Exception processing file: " + file_path + " - " + std::string(e.what()));
                            event.success = false;
                            event.error_message = "Exception: " + std::string(e.what());
                            processed_count.fetch_add(1);
                            failed_processed.fetch_add(1);
                            if (onNext) onNext(event);
                        }
                    }
            
            // Log final statistics
            Logger::info("Processing completed - Total: " + std::to_string(processed_count.load()) + 
                        ", Successful: " + std::to_string(successful_processed.load()) + 
                        ", Failed: " + std::to_string(failed_processed.load()));
            
            if (onComplete) onComplete();
            
        } catch (const std::exception& e) {
            Logger::error("Fatal error in processAllScannedFiles: " + std::string(e.what()));
            if (onError) onError(e);
        } });
}

void MediaProcessingOrchestrator::cancel()
{
    cancelled_.store(true);
    Logger::info("Processing cancellation requested");
}

void MediaProcessingOrchestrator::startTimerBasedProcessing(int processing_interval_seconds, int max_threads)
{
    std::lock_guard<std::mutex> lock(processing_mutex_);

    if (timer_processing_running_.load())
    {
        Logger::warn("Timer-based processing is already running");
        return;
    }

    // Use configuration if max_threads is -1 (default)
    if (max_threads == -1)
    {
        auto &config_manager = ServerConfigManager::getInstance();
        max_threads = config_manager.getMaxProcessingThreads();
    }

    Logger::info("Starting timer-based processing with interval: " + std::to_string(processing_interval_seconds) + " seconds");

    timer_processing_running_.store(true);
    cancelled_.store(false);

    // Start processing thread
    processing_thread_ = std::thread(&MediaProcessingOrchestrator::processingThreadFunction, this, processing_interval_seconds, max_threads);
}

void MediaProcessingOrchestrator::stopTimerBasedProcessing()
{
    std::lock_guard<std::mutex> lock(processing_mutex_);

    if (!timer_processing_running_.load())
    {
        return;
    }

    Logger::info("Stopping timer-based processing");

    // Signal thread to stop
    timer_processing_running_.store(false);
    cancelled_.store(true);
    processing_cv_.notify_all();

    // Wait for thread to complete
    if (processing_thread_.joinable())
    {
        processing_thread_.join();
    }

    Logger::info("Timer-based processing stopped");
}

void MediaProcessingOrchestrator::onConfigChanged(const ConfigEvent &event)
{
    if (event.type == ConfigEventType::DEDUP_MODE_CHANGED)
    {
        std::cout << "[CONFIG CHANGE] MediaProcessingOrchestrator: Deduplication mode changed from " +
                         event.old_value.as<std::string>() + " to " +
                         event.new_value.as<std::string>() + " - will use new mode for future processing"
                  << std::endl;

        Logger::info("MediaProcessingOrchestrator: Deduplication mode changed from " +
                     event.old_value.as<std::string>() + " to " +
                     event.new_value.as<std::string>() + " - will use new mode for future processing");

        // Note: We don't need to restart processing here since each processing run
        // queries the current mode via ServerConfigManager::getInstance().getDedupMode()
        // This ensures that new processing runs will use the updated mode
    }
}

void MediaProcessingOrchestrator::setScanningInProgress(bool in_progress)
{
    scanning_in_progress_.store(in_progress);
    if (in_progress)
    {
        Logger::info("Scanning in progress - processing will continue to run concurrently");
    }
    else
    {
        Logger::info("Scanning completed - processing continues to run independently");
    }
}

bool MediaProcessingOrchestrator::isTimerBasedProcessingRunning() const
{
    return timer_processing_running_.load();
}

void MediaProcessingOrchestrator::processingThreadFunction(int processing_interval_seconds, int max_threads)
{
    Logger::info("Processing thread started with interval: " + std::to_string(processing_interval_seconds) + " seconds");

    while (timer_processing_running_.load() && !cancelled_.load())
    {
        // Check if we should stop
        if (!timer_processing_running_.load() || cancelled_.load())
        {
            break;
        }

        // Use a mutex to ensure only one processing run happens at a time
        std::unique_lock<std::mutex> processing_run_lock(processing_mutex_);

        Logger::info("Starting scheduled processing run");

        try
        {
            // Process files that need processing
            auto observable = processAllScannedFiles(max_threads);

            // Use shared_ptr for thread-safe access to counters
            auto processed_count = std::make_shared<size_t>(0);
            auto successful_count = std::make_shared<size_t>(0);
            auto failed_count = std::make_shared<size_t>(0);

            observable.subscribe(
                [processed_count, successful_count, failed_count](const FileProcessingEvent &event)
                {
                    (*processed_count)++;
                    if (event.success)
                    {
                        (*successful_count)++;
                    }
                    else
                    {
                        (*failed_count)++;
                    }
                    Logger::debug("Processed file: " + event.file_path + " (success: " + std::to_string(event.success) + ")");
                },
                [](const std::exception &e)
                {
                    Logger::error("Processing error: " + std::string(e.what()));
                },
                [processed_count, successful_count, failed_count]()
                {
                    Logger::info("Scheduled processing run completed. Processed: " + std::to_string(*processed_count) +
                                 ", Successful: " + std::to_string(*successful_count) +
                                 ", Failed: " + std::to_string(*failed_count));
                });
        }
        catch (const std::exception &e)
        {
            Logger::error("Exception in scheduled processing: " + std::string(e.what()));
        }

        // Wait for next processing interval
        if (timer_processing_running_.load() && !cancelled_.load())
        {
            Logger::debug("Waiting " + std::to_string(processing_interval_seconds) + " seconds until next processing run");

            std::unique_lock<std::mutex> lock(processing_mutex_);
            processing_cv_.wait_for(lock, std::chrono::seconds(processing_interval_seconds), [this]
                                    { return !timer_processing_running_.load() || cancelled_.load(); });
        }
    }

    Logger::info("Processing thread stopped");
}