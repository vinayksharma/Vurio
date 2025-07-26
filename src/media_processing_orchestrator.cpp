#include "core/media_processing_orchestrator.hpp"
#include "core/media_processor.hpp"
#include "core/file_utils.hpp"
#include "core/server_config_manager.hpp"
#include "logging/logger.hpp"
#include <chrono>
#include <thread>
#include <condition_variable>
#include <tbb/parallel_for.h>
#include <tbb/blocked_range.h>
#include <tbb/mutex.h>

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
            if (mode != DedupMode::FAST && mode != DedupMode::BALANCED && mode != DedupMode::QUALITY) {
                Logger::error("Invalid dedup mode configured");
                if (onError) onError(std::runtime_error("Invalid dedup mode"));
                return;
            }
            
            Logger::info("Starting file processing with mode: " + DedupModes::getModeName(mode));
            
            auto files_to_process = dbMan_.getFilesNeedingProcessing(mode);
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
                        
                        auto start = std::chrono::steady_clock::now();
                        FileProcessingEvent event;
                        event.file_path = files_to_process[i].first;
                        
                        try {
                            // Check if file is supported
                            if (!MediaProcessor::isSupportedFile(files_to_process[i].first)) {
                                event.success = false;
                                event.error_message = "Unsupported file type: " + files_to_process[i].first;
                                Logger::error(event.error_message);
                                processed_files.fetch_add(1);
                                failed_files.fetch_add(1);
                                if (onNext) onNext(event);
                                continue;
                            }
                            
                            // Process the file
                            ProcessingResult result = MediaProcessor::processFile(files_to_process[i].first, mode);
                            event.artifact_format = result.artifact.format;
                            event.artifact_hash = result.artifact.hash;
                            event.artifact_confidence = result.artifact.confidence;
                            
                            if (!result.success) {
                                event.success = false;
                                event.error_message = result.error_message;
                                Logger::error("Processing failed for: " + files_to_process[i].first + " - " + result.error_message);
                                processed_files.fetch_add(1);
                                failed_files.fetch_add(1);
                                if (onNext) onNext(event);
                                continue;
                            }
                            
                            // Thread-safe database operations
                            bool store_success = false;
                            bool hash_success = false;
                            std::string db_error_msg;
                            
                            // Lock database operations to prevent race conditions
                            tbb::mutex::scoped_lock lock(db_mutex);
                            
                            // Store processing result
                            auto [store_result, store_op_id] = dbMan_.storeProcessingResultWithId(files_to_process[i].first, mode, result);
                            if (!store_result.success) {
                                db_error_msg = store_result.error_message;
                                Logger::error("Failed to enqueue processing result: " + files_to_process[i].first + " - " + db_error_msg);
                                processed_files.fetch_add(1);
                                failed_files.fetch_add(1);
                                event.success = false;
                                event.error_message = "Database operation failed: " + db_error_msg;
                                if (onNext) onNext(event);
                                continue;
                            }
                            
                            // Wait for write queue to process the store operation
                            dbMan_.waitForWrites();
                            
                            // Check if the store operation actually succeeded
                            auto store_op_result = dbMan_.getAccessQueue().getOperationResult(store_op_id);
                            if (!store_op_result.success) {
                                Logger::error("Store operation failed: " + files_to_process[i].first + " - " + store_op_result.error_message);
                                processed_files.fetch_add(1);
                                failed_files.fetch_add(1);
                                event.success = false;
                                event.error_message = "Database operation failed: " + store_op_result.error_message;
                                if (onNext) onNext(event);
                                continue;
                            }
                            
                            // Generate file hash and update database
                            std::string file_hash = FileUtils::computeFileHash(files_to_process[i].first);
                            auto [hash_result, hash_op_id] = dbMan_.updateFileHashWithId(files_to_process[i].first, file_hash);
                            if (!hash_result.success) {
                                db_error_msg = hash_result.error_message;
                                Logger::error("Failed to enqueue hash update: " + files_to_process[i].first + " - " + db_error_msg);
                                processed_files.fetch_add(1);
                                failed_files.fetch_add(1);
                                event.success = false;
                                event.error_message = "Database operation failed: " + db_error_msg;
                                if (onNext) onNext(event);
                                continue;
                            }
                            
                            // Wait for write queue to process the hash update
                            dbMan_.waitForWrites();
                            
                            // Check if the hash update operation actually succeeded
                            auto hash_op_result = dbMan_.getAccessQueue().getOperationResult(hash_op_id);
                            if (!hash_op_result.success) {
                                Logger::error("Hash update operation failed: " + files_to_process[i].first + " - " + hash_op_result.error_message);
                                processed_files.fetch_add(1);
                                failed_files.fetch_add(1);
                                event.success = false;
                                event.error_message = "Database operation failed: " + hash_op_result.error_message;
                                if (onNext) onNext(event);
                                continue;
                            }
                            
                            // Success - update counters and emit event
                            auto end = std::chrono::steady_clock::now();
                            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
                            event.processing_time_ms = duration.count();
                            event.success = true;
                            
                            processed_files.fetch_add(1);
                            successful_files.fetch_add(1);
                            
                            Logger::info("Successfully processed file: " + files_to_process[i].first + 
                                       " (format: " + event.artifact_format + 
                                       ", confidence: " + std::to_string(event.artifact_confidence) + 
                                       ", time: " + std::to_string(event.processing_time_ms) + "ms)");
                            
                            if (onNext) onNext(event);
                            
                        } catch (const std::exception& e) {
                            Logger::error("Exception processing file: " + files_to_process[i].first + " - " + std::string(e.what()));
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