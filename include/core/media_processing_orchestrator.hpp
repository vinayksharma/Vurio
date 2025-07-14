#pragma once

#include <string>
#include <vector>
#include <functional>
#include <chrono>
#include <atomic>
#include "file_utils.hpp"
#include "database_manager.hpp"
#include "media_processor.hpp"
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
    explicit MediaProcessingOrchestrator(DatabaseManager &db);
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

private:
    DatabaseManager &db_;
    std::atomic<bool> cancelled_;
};