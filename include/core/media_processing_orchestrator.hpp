#pragma once

#include "database/database_manager.hpp"
#include "media_processor.hpp"
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <chrono>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include "file_utils.hpp"
#include "server_config_manager.hpp"

struct FileProcessingEvent
{
    std::string file_path;
    bool success;
    std::string error_message;
    long long processing_time_ms = 0;
    std::string artifact_format;
    std::string artifact_hash;
    double artifact_confidence = 0.0;
};

/**
 * @brief Orchestrates processing of scanned media files with standardized error handling.
 *
 * Error Handling Policy:
 * - All errors are logged with context.
 * - Per-file errors (e.g., unsupported file, processing failure, DB error) are emitted via onNext with success=false.
 * - Fatal errors (e.g., DB not available, invalid config, cancellation) are emitted via onError.
 * - No silent failures: all errors are both logged and reported.
 */
class MediaProcessingOrchestrator
{
public:
    explicit MediaProcessingOrchestrator(DatabaseManager &dbMan);
    ~MediaProcessingOrchestrator();

    /**
     * @brief Process files that need processing (those without hash) in parallel.
     *
     * This method processes files that don't have a hash in the database,
     * indicating they haven't been processed yet or need reprocessing.
     * Files are processed in parallel using the specified number of threads.
     * Processing results are stored in the database and file hashes are updated.
     *
     * @param max_threads Maximum number of threads to use for processing
     * @return Observable that emits FileProcessingEvent for each processed file
     */
    SimpleObservable<FileProcessingEvent> processAllScannedFiles(int max_threads = 4);

    /**
     * @brief Cancel ongoing processing operations
     * This method sets a cancellation flag that will be checked by processing threads
     */
    void cancel();

    /**
     * @brief Start timer-based processing with scanning coordination
     *
     * This method starts a background thread that processes files at regular intervals.
     * The processing waits for any ongoing scanning operations to complete before starting.
     *
     * @param processing_interval_seconds Interval between processing runs in seconds (default: 60)
     * @param max_threads Maximum number of threads to use for processing (default: 4)
     */
    void startTimerBasedProcessing(int processing_interval_seconds = 60, int max_threads = 4);

    /**
     * @brief Stop timer-based processing
     *
     * This method stops the background processing thread and waits for it to complete.
     */
    void stopTimerBasedProcessing();

    /**
     * @brief Set scanning in progress flag
     *
     * This method should be called when scanning starts to track scanning status.
     * Scanning and processing can now run concurrently.
     */
    void setScanningInProgress(bool in_progress);

    /**
     * @brief Check if timer-based processing is currently running
     *
     * @return true if timer-based processing is active, false otherwise
     */
    bool isTimerBasedProcessingRunning() const;

private:
    DatabaseManager &dbMan_;
    std::atomic<bool> cancelled_;

    // Timer-based processing members
    std::atomic<bool> timer_processing_running_{false};
    std::atomic<bool> scanning_in_progress_{false};
    std::thread processing_thread_;
    std::mutex processing_mutex_;
    std::condition_variable processing_cv_;

    /**
     * @brief Background processing thread function
     *
     * This function runs in a separate thread and processes files at regular intervals.
     * It waits for scanning to complete before starting processing.
     */
    void processingThreadFunction(int processing_interval_seconds, int max_threads);
};