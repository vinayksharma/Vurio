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
    return SimpleObservable<FileProcessingEvent>([this, max_threads](auto onNext, auto onError, auto onComplete)
                                                 {
        try {
            // Reset cancellation flag
            cancelled_ = false;
            
            // Validate database connection
            if (!db_.isValid()) {
                Logger::error("Database not initialized or invalid");
                if (onError) onError(std::runtime_error("Database not initialized"));
                return;
            }
            
            auto scanned_files = db_.getAllUnprocessedScannedFiles();
            if (scanned_files.empty()) {
                Logger::info("No unprocessed files found");
                if (onComplete) onComplete();
                return;
            }
            
            auto& config_manager = ServerConfigManager::getInstance();
            DedupMode mode = config_manager.getDedupMode();
            
            // Validate dedup mode
            if (mode != DedupMode::FAST && mode != DedupMode::BALANCED && mode != DedupMode::QUALITY) {
                Logger::error("Invalid dedup mode configured");
                if (onError) onError(std::runtime_error("Invalid dedup mode"));
                return;
            }
            
            Logger::info("Starting processing of " + std::to_string(scanned_files.size()) + 
                        " files with " + std::to_string(max_threads) + " threads");
            
            // Separate mutexes for different operations to reduce contention
            std::mutex store_mutex;
            std::mutex mark_mutex;
            std::mutex progress_mutex;
            
            // Progress tracking
            std::atomic<size_t> total_files{scanned_files.size()};
            std::atomic<size_t> processed_files{0};
            std::atomic<size_t> successful_files{0};
            std::atomic<size_t> failed_files{0};
            
            // RAII for futures management
            class FutureManager {
                std::vector<std::future<void>>& futures_;
            public:
                explicit FutureManager(std::vector<std::future<void>>& futures) : futures_(futures) {}
                ~FutureManager() {
                    for (auto& f : futures_) {
                        if (f.valid()) {
                            try {
                                f.wait();
                            } catch (const std::exception& e) {
                                Logger::error("Exception in future: " + std::string(e.what()));
                            }
                        }
                    }
                }
            };
            
            std::vector<std::future<void>> futures;
            FutureManager future_manager(futures);
            
            auto process_file = [&](const std::string& file_path) {
                // Check for cancellation
                if (cancelled_.load()) {
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
                        
                        // Update progress
                        processed_files.fetch_add(1);
                        failed_files.fetch_add(1);
                        
                        if (onNext) onNext(event);
                        return;
                    }
                    
                    ProcessingResult result;
                    try {
                        result = MediaProcessor::processFile(file_path, mode);
                    } catch (const std::exception& e) {
                        event.success = false;
                        event.error_message = "Exception during processing: " + std::string(e.what());
                        Logger::error(event.error_message);
                        
                        // Update progress
                        processed_files.fetch_add(1);
                        failed_files.fetch_add(1);
                        
                        if (onNext) onNext(event);
                        return;
                    }
                    
                    auto end = std::chrono::steady_clock::now();
                    event.processing_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
                    event.artifact_format = result.artifact.format;
                    event.artifact_hash = result.artifact.hash;
                    event.artifact_confidence = result.artifact.confidence;
                    
                    if (!result.success) {
                        event.success = false;
                        event.error_message = result.error_message;
                        Logger::error("Processing failed for: " + file_path + " - " + result.error_message);
                        
                        // Update progress
                        processed_files.fetch_add(1);
                        failed_files.fetch_add(1);
                    } else {
                        event.success = true;
                        event.error_message = "";
                        
                        // Store result in DB (thread-safe with separate mutexes)
                        bool store_success = false;
                        bool mark_success = false;
                        
                        {
                            std::lock_guard<std::mutex> store_lock(store_mutex);
                            store_success = db_.storeProcessingResult(file_path, mode, result);
                        }
                        
                        if (store_success) {
                            std::lock_guard<std::mutex> mark_lock(mark_mutex);
                            mark_success = db_.markFileAsProcessed(file_path);
                        }
                        
                        if (!store_success || !mark_success) {
                            Logger::error("Failed to store processing result or mark file as processed: " + file_path);
                            event.success = false;
                            event.error_message = "Database operation failed";
                            
                            // Update progress
                            processed_files.fetch_add(1);
                            failed_files.fetch_add(1);
                        } else {
                            // Update progress
                            processed_files.fetch_add(1);
                            successful_files.fetch_add(1);
                        }
                    }
                    
                    if (onNext) onNext(event);
                    
                } catch (const std::exception& e) {
                    Logger::error("Unexpected error processing file " + file_path + ": " + std::string(e.what()));
                    
                    // Update progress
                    processed_files.fetch_add(1);
                    failed_files.fetch_add(1);
                    
                    if (onNext) {
                        FileProcessingEvent error_event;
                        error_event.file_path = file_path;
                        error_event.success = false;
                        error_event.error_message = "Unexpected error: " + std::string(e.what());
                        onNext(error_event);
                    }
                }
            };
            
            // Launch up to max_threads in parallel
            for (const auto& [file_path, file_name] : scanned_files) {
                // Check for cancellation
                if (cancelled_.load()) {
                    Logger::info("Processing cancelled");
                    break;
                }
                
                if (max_threads > 1) {
                    futures.emplace_back(std::async(std::launch::async, process_file, file_path));
                    if (futures.size() >= static_cast<size_t>(max_threads)) {
                        for (auto& f : futures) {
                            if (f.valid()) {
                                try {
                                    f.get();
                                } catch (const std::exception& e) {
                                    Logger::error("Exception in processing thread: " + std::string(e.what()));
                                }
                            }
                        }
                        futures.clear();
                    }
                } else {
                    process_file(file_path);
                }
            }
            
            // Wait for any remaining futures
            for (auto& f : futures) {
                if (f.valid()) {
                    try {
                        f.get();
                    } catch (const std::exception& e) {
                        Logger::error("Exception in processing thread: " + std::string(e.what()));
                    }
                }
            }
            
            // Log final statistics
            Logger::info("Processing completed: " + std::to_string(processed_files.load()) + 
                        " processed, " + std::to_string(successful_files.load()) + 
                        " successful, " + std::to_string(failed_files.load()) + " failed");
            
            if (onComplete) onComplete();
            
        } catch (const std::exception& e) {
            Logger::error("Fatal error in orchestrator: " + std::string(e.what()));
            if (onError) onError(e);
        } });
}

void MediaProcessingOrchestrator::cancel()
{
    cancelled_.store(true);
    Logger::info("Processing cancellation requested");
}