#include "core/media_processing_orchestrator.hpp"
#include "logging/logger.hpp"
#include <future>
#include <mutex>
#include <atomic>
#include <chrono>

MediaProcessingOrchestrator::MediaProcessingOrchestrator(const std::string &db_path)
    : db_path_(db_path), db_(db_path), cancelled_(false) {}

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
            if (!db_.isValid()) {
                Logger::error("Database not initialized or invalid");
                if (onError) onError(std::runtime_error("Database not initialized"));
                return;
            }
            auto files_to_process = db_.getFilesNeedingProcessing();
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
            std::mutex store_mutex;
            std::mutex hash_mutex;
            std::atomic<size_t> total_files{files_to_process.size()};
            std::atomic<size_t> processed_files{0};
            std::atomic<size_t> successful_files{0};
            std::atomic<size_t> failed_files{0};
            class FutureManager {
                std::vector<std::future<void>>& futures_;
            public:
                explicit FutureManager(std::vector<std::future<void>>& futures) : futures_(futures) {}
                ~FutureManager() {
                    for (auto& f : futures_) {
                        if (f.valid()) {
                            try { f.wait(); }
                            catch (const std::exception& e) {
                                Logger::error("Exception in future: " + std::string(e.what()));
                            }
                        }
                    }
                }
            };
            std::vector<std::future<void>> futures;
            FutureManager future_manager(futures);
            auto process_file = [&](const std::string& file_path)
            {
                if (cancelled_.load()) {
                    Logger::warn("Processing cancelled before file: " + file_path);
                    if (onError) onError(std::runtime_error("Processing cancelled"));
                    return;
                }
                auto start = std::chrono::steady_clock::now();
                FileProcessingEvent event;
                event.file_path = file_path;
                try {
                    if (!MediaProcessor::isSupportedFile(file_path)) {
                        event.success = false;
                        event.error_message = "Unsupported file type: " + file_path;
                        Logger::error(event.error_message);
                        processed_files.fetch_add(1);
                        failed_files.fetch_add(1);
                        if (onNext) onNext(event);
                        return;
                    }
                    ProcessingResult result = MediaProcessor::processFile(file_path, mode);
                    event.artifact_format = result.artifact.format;
                    event.artifact_hash = result.artifact.hash;
                    event.artifact_confidence = result.artifact.confidence;
                    if (!result.success) {
                        event.success = false;
                        event.error_message = result.error_message;
                        Logger::error("Processing failed for: " + file_path + " - " + result.error_message);
                        processed_files.fetch_add(1);
                        failed_files.fetch_add(1);
                        if (onNext) onNext(event);
                        return;
                    }
                    bool store_success = false;
                    bool hash_success = false;
                    std::string db_error_msg;
                    {
                        std::lock_guard<std::mutex> store_lock(store_mutex);
                        DBOpResult store_result = db_.storeProcessingResult(file_path, mode, result);
                        store_success = store_result.success;
                        if (!store_success) db_error_msg = store_result.error_message;
                    }
                    if (store_success) {
                        // Generate file hash and update database
                        std::string file_hash = FileUtils::computeFileHash(file_path);
                        {
                            std::lock_guard<std::mutex> hash_lock(hash_mutex);
                            DBOpResult hash_result = db_.updateFileHash(file_path, file_hash);
                            hash_success = hash_result.success;
                            if (!hash_success) db_error_msg = hash_result.error_message;
                        }
                    }
                    if (!store_success || !hash_success) {
                        Logger::error("Failed to store processing result or update hash: " + file_path + ". DB error: " + db_error_msg);
                        event.success = false;
                        event.error_message = "Database operation failed: " + db_error_msg;
                        processed_files.fetch_add(1);
                        failed_files.fetch_add(1);
                        if (onNext) onNext(event);
                        return;
                    }
                    auto end = std::chrono::steady_clock::now();
                    event.processing_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
                    event.success = true;
                    event.error_message = "";
                    processed_files.fetch_add(1);
                    successful_files.fetch_add(1);
                    if (onNext) onNext(event);
                }
                catch (const std::exception& e) {
                    Logger::error("Unexpected error processing file " + file_path + ": " + std::string(e.what()));
                    event.success = false;
                    event.error_message = "Unexpected error: " + std::string(e.what());
                    processed_files.fetch_add(1);
                    failed_files.fetch_add(1);
                    if (onNext) onNext(event);
                }
            };
            for (const auto& [file_path, file_name] : files_to_process) {
                if (cancelled_.load()) {
                    Logger::info("Processing cancelled");
                    if (onError) onError(std::runtime_error("Processing cancelled"));
                    return;
                }
                if (max_threads > 1) {
                    futures.emplace_back(std::async(std::launch::async, process_file, file_path));
                    if (futures.size() >= static_cast<size_t>(max_threads)) {
                        for (auto& f : futures) {
                            if (f.valid()) {
                                try { f.get(); }
                                catch (const std::exception& e) {
                                    Logger::error("Exception in processing thread: " + std::string(e.what()));
                                    if (onError) onError(e);
                                }
                            }
                        }
                        futures.clear();
                    }
                } else {
                    process_file(file_path);
                }
            }
            for (auto& f : futures) {
                if (f.valid()) {
                    try { f.get(); }
                    catch (const std::exception& e) {
                        Logger::error("Exception in processing thread: " + std::string(e.what()));
                        if (onError) onError(e);
                    }
                }
            }
            Logger::info("Processing completed. Total: " + std::to_string(total_files.load()) + 
                        ", Processed: " + std::to_string(processed_files.load()) + 
                        ", Successful: " + std::to_string(successful_files.load()) + 
                        ", Failed: " + std::to_string(failed_files.load()));
            if (onComplete) onComplete();
        }
        catch (const std::exception& e) {
            Logger::error("Fatal error in processAllScannedFiles: " + std::string(e.what()));
            if (onError) onError(e);
        } });
}

void MediaProcessingOrchestrator::cancel()
{
    cancelled_.store(true);
    Logger::info("Processing cancellation requested");
}