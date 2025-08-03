#include "core/media_processing_orchestrator.hpp"
#include "core/media_processor.hpp"
#include "core/transcoding_manager.hpp"
#include "database/database_manager.hpp"
#include "logging/logger.hpp"
#include <chrono>
#include <thread>
#include <condition_variable>
#include <tbb/parallel_for.h>
#include <tbb/blocked_range.h>
#include <tbb/mutex.h>
#include <filesystem>

MediaProcessingOrchestrator::MediaProcessingOrchestrator(DatabaseManager &dbMan)
    : dbMan_(dbMan), cancelled_(false) {}

MediaProcessingOrchestrator::~MediaProcessingOrchestrator()
{
    // Ensure cancellation is set to prevent any ongoing operations
    cancelled_.store(true);

    // Stop timer-based processing if running
    stopTimerBasedProcessing();

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
            if (pre_process_quality_stack) {
                // Get files that need processing for any mode using atomic batch processing
                // This prevents duplicates by atomically marking files as in progress
                files_to_process = dbMan_.getAndMarkFilesForProcessing(DedupMode::FAST, 50);
                auto balanced_files = dbMan_.getAndMarkFilesForProcessing(DedupMode::BALANCED, 50);
                auto quality_files = dbMan_.getAndMarkFilesForProcessing(DedupMode::QUALITY, 50);
                
                // Combine all files (avoiding duplicates)
                std::set<std::string> unique_files;
                for (const auto& [path, name] : files_to_process) {
                    unique_files.insert(path);
                }
                for (const auto& [path, name] : balanced_files) {
                    if (unique_files.find(path) == unique_files.end()) {
                        files_to_process.emplace_back(path, name);
                        unique_files.insert(path);
                    }
                }
                for (const auto& [path, name] : quality_files) {
                    if (unique_files.find(path) == unique_files.end()) {
                        files_to_process.emplace_back(path, name);
                        unique_files.insert(path);
                    }
                }
            } else {
                // Get files that need processing for the specific mode using atomic batch processing
                files_to_process = dbMan_.getAndMarkFilesForProcessing(mode, 50);
            }
            
            if (files_to_process.empty()) {
                Logger::info("No files need processing");
                if (onComplete) onComplete();
                return;
            }
            
            Logger::info("Starting processing of " + std::to_string(files_to_process.size()) + 
                        " files with " + std::to_string(actual_max_threads) + " threads");
            
            // Thread-safe counters for progress tracking
            std::atomic<size_t> processed_files{0};
            std::atomic<size_t> successful_files{0};
            std::atomic<size_t> failed_files{0};
            
            // Thread-safe mutex for database operations to prevent race conditions
            tbb::mutex db_mutex;
            
            // Process files in parallel using TBB
            tbb::parallel_for(tbb::blocked_range<size_t>(0, files_to_process.size()),
                [&](const tbb::blocked_range<size_t>& range) {
                    for (size_t i = range.begin(); i != range.end(); ++i) {
                        // Check for cancellation
                        if (cancelled_.load()) {
                            return; // Exit this thread's processing
                        }
                        
                        const std::string& file_path = files_to_process[i].first;
                        
                        // Process the file for each required mode using database atomic locking
                        // No need for in-memory locking since database atomic operations handle concurrency
                        
                        auto start = std::chrono::steady_clock::now();
                        FileProcessingEvent event;
                        event.file_path = file_path;
                        
                        try {
                            // Check if file is supported
                            if (!MediaProcessor::isSupportedFile(file_path)) {
                                event.success = false;
                                event.error_message = "Unsupported file type: " + file_path;
                                Logger::error(event.error_message);
                                processed_files.fetch_add(1);
                                failed_files.fetch_add(1);
                                if (onNext) onNext(event);
                                continue;
                            }
                            
                            // Process the file for each required mode
                            // Files are already marked as in progress (-1) by getAndMarkFilesForProcessing
                            bool any_success = false;
                            std::string last_error;
                            
                            for (const auto& process_mode : modes_to_process) {
                                // Check if this file needs processing for this mode
                                // Files with status -1 (in progress) should be processed
                                if (!dbMan_.fileNeedsProcessingForMode(file_path, process_mode)) {
                                    Logger::debug("Skipping file " + file_path + " for mode " + DedupModes::getModeName(process_mode) + " - already processed");
                                    continue;
                                }
                                
                                Logger::info("Processing file: " + file_path + " with mode: " + DedupModes::getModeName(process_mode));
                                
                                // Check if this file has a transcoded version available
                                std::string actual_file_path = file_path;
                                std::string transcoded_path = TranscodingManager::getInstance().getTranscodedFilePath(file_path);
                                if (!transcoded_path.empty() && std::filesystem::exists(transcoded_path))
                                {
                                    actual_file_path = transcoded_path;
                                    Logger::info("Using transcoded file for processing: " + file_path + " -> " + transcoded_path);
                                }
                                
                                // Process the file for this mode
                                ProcessingResult result = MediaProcessor::processFile(actual_file_path, process_mode);
                                
                                if (result.success) {
                                    any_success = true;
                                    // Update event with the most recent successful result
                                    event.artifact_format = result.artifact.format;
                                    event.artifact_hash = result.artifact.hash;
                                    event.artifact_confidence = result.artifact.confidence;
                                    
                                    // Thread-safe database operations
                                    bool store_success = false;
                                    std::string db_error_msg;
                                    
                                    // Lock database operations to prevent race conditions
                                    tbb::mutex::scoped_lock lock(db_mutex);
                                    
                                    // Store processing result for this mode
                                    auto [store_result, store_op_id] = dbMan_.storeProcessingResultWithId(file_path, process_mode, result);
                                    if (!store_result.success) {
                                        db_error_msg = store_result.error_message;
                                        Logger::error("Failed to store processing result for mode " + DedupModes::getModeName(process_mode) + ": " + file_path + " - " + db_error_msg);
                                    } else {
                                        Logger::info("Successfully processed and stored result for " + file_path + " with mode " + DedupModes::getModeName(process_mode));
                                        
                                        // Mark as completed (change from -1 to 1)
                                        dbMan_.setProcessingFlag(file_path, process_mode);
                                        Logger::debug("Processing completed for " + file_path + " with mode " + DedupModes::getModeName(process_mode));
                                    }
                                } else {
                                    last_error = result.error_message;
                                    Logger::error("Processing failed for " + file_path + " with mode " + DedupModes::getModeName(process_mode) + " - " + result.error_message);
                                    
                                    // Mark as failed (change from -1 to 0 to allow retry)
                                    dbMan_.setProcessingFlag(file_path, process_mode);
                                }
                            }
                            
                            // Set final event result
                            if (any_success) {
                                event.success = true;
                                successful_files.fetch_add(1);
                            } else {
                                event.success = false;
                                event.error_message = "Processing failed for all modes: " + last_error;
                                failed_files.fetch_add(1);
                            }
                            
                            // Success - update counters and emit event
                            auto end = std::chrono::steady_clock::now();
                            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
                            event.processing_time_ms = duration.count();
                            
                            processed_files.fetch_add(1);
                            
                            Logger::info("Successfully processed file: " + file_path + 
                                       " (format: " + event.artifact_format + 
                                       ", confidence: " + std::to_string(event.artifact_confidence) + 
                                       ", time: " + std::to_string(event.processing_time_ms) + "ms)");
                            
                            if (onNext) onNext(event);
                            
                        } catch (const std::exception& e) {
                            Logger::error("Exception processing file: " + file_path + " - " + std::string(e.what()));
                            event.success = false;
                            event.error_message = "Exception: " + std::string(e.what());
                            processed_files.fetch_add(1);
                            failed_files.fetch_add(1);
                            if (onNext) onNext(event);
                        }
                    }
                });
            
            // Log final statistics
            Logger::info("Processing completed - Total: " + std::to_string(processed_files.load()) + 
                        ", Successful: " + std::to_string(successful_files.load()) + 
                        ", Failed: " + std::to_string(failed_files.load()));
            
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