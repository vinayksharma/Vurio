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

class MediaProcessingOrchestrator
{
public:
    explicit MediaProcessingOrchestrator(const std::string &db_path);
    SimpleObservable<FileProcessingEvent> processAllScannedFiles(int max_threads = 4);

    /**
     * @brief Cancel ongoing processing operations
     * This method sets a cancellation flag that will be checked by processing threads
     */
    void cancel();

private:
    std::string db_path_;
    DatabaseManager db_;
    std::atomic<bool> cancelled_;
};