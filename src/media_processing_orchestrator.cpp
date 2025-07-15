#include "core/media_processing_orchestrator.hpp"
#include "logging/logger.hpp"
#include <future>
#include <mutex>
#include <atomic>
#include <chrono>

MediaProcessingOrchestrator::MediaProcessingOrchestrator(DatabaseManager &dbMan)
    : dbMan_(dbMan), cancelled_(false) {}

MediaProcessingOrchestrator::~MediaProcessingOrchestrator()
{
    // Ensure cancellation is set to prevent any ongoing operations
    cancelled_.store(true);
    Logger::debug("MediaProcessingOrchestrator destructor called");
}

SimpleObservable<FileProcessingEvent> MediaProcessingOrchestrator::processAllScannedFiles(int max_threads)
{
    // Error handling policy:
    // - All errors are logged with context
    // - Per-file errors are emitted via onNext (with success=false)
    // - Fatal errors (DB not available, invalid config, cancellation) are emitted via onError
    // - No silent failures
    return SimpleObservable<FileProcessingEvent>([this, max_threads](auto onNext, auto onError, auto onComplete)
                                                 {
        try {
            cancelled_ = false;
            if (!dbMan_.isValid()) {
                Logger::error("Database not initialized or invalid");
                if (onError) onError(std::runtime_error("Database not initialized"));
                return;
            }
            auto files_to_process = dbMan_.getFilesNeedingProcessing();
            if (files_to_process.empty()) {
                Logger::info("No files need processing");
                if (onComplete) onComplete();
                return;
            }
            auto& config_manager = ServerConfigManager::getInstance();
            DedupMode mode = config_manager.getDedupMode();
            if (mode != DedupMode::FAST && mode != DedupMode::BALANCED && mode != DedupMode::QUALITY) {
                Logger::error("Invalid dedup mode configured");
                if (onError) onError(std::runtime_error("Invalid dedup mode"));
                return;
            }
            Logger::info("Starting processing of " + std::to_string(files_to_process.size()) + 
                        " files with " + std::to_string(max_threads) + " threads");
            std::atomic<size_t> total_files{files_to_process.size()};
            std::atomic<size_t> processed_files{0};
            std::atomic<size_t> successful_files{0};
            std::atomic<size_t> failed_files{0};

            // Process files sequentially (TBB not available)
            for (size_t i = 0; i < files_to_process.size(); ++i)
            {
                if (cancelled_.load()) {
                    Logger::warn("Processing cancelled before file: " + files_to_process[i].first);
                    if (onError) onError(std::runtime_error("Processing cancelled"));
                    return;
                }
                auto start = std::chrono::steady_clock::now();
                FileProcessingEvent event;
                event.file_path = files_to_process[i].first;
                try {
                    if (!MediaProcessor::isSupportedFile(files_to_process[i].first)) {
                        event.success = false;
                        event.error_message = "Unsupported file type: " + files_to_process[i].first;
                        Logger::error(event.error_message);
                        processed_files.fetch_add(1);
                        failed_files.fetch_add(1);
                        if (onNext) onNext(event);
                        continue;
                    }
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
                    bool store_success = false;
                    bool hash_success = false;
                    std::string db_error_msg;
                    
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
                    
                    // Both operations succeeded
                    store_success = true;
                    hash_success = true;
                    
                    auto end = std::chrono::steady_clock::now();
                    event.processing_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
                    event.success = true;
                    event.error_message = "";
                    processed_files.fetch_add(1);
                    successful_files.fetch_add(1);
                    Logger::info("Stored processing result for: " + files_to_process[i].first);
                    if (onNext) onNext(event);
                }
                catch (const std::exception &e) {
                    Logger::error("Exception processing file: " + files_to_process[i].first + " - " + std::string(e.what()));
                    processed_files.fetch_add(1);
                    failed_files.fetch_add(1);
                    event.success = false;
                    event.error_message = "Exception: " + std::string(e.what());
                    if (onNext) onNext(event);
                }
            }

            // Processing is sequential, so all files are already processed
            // No need to wait for completion

            Logger::info("Processing completed. Total: " + std::to_string(total_files.load()) + 
                        ", Processed: " + std::to_string(processed_files.load()) + 
                        ", Successful: " + std::to_string(successful_files.load()) + 
                        ", Failed: " + std::to_string(failed_files.load()));

            if (onComplete) onComplete();
        }
        catch (const std::exception &e) {
            Logger::error("Fatal error in processing: " + std::string(e.what()));
            if (onError) onError(e);
        } });
}

void MediaProcessingOrchestrator::cancel()
{
    cancelled_.store(true);
    Logger::info("Processing cancellation requested");
}