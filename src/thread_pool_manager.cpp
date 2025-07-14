#include "core/thread_pool_manager.hpp"
#include "core/media_processor.hpp"
#include "core/server_config_manager.hpp"
#include <tbb/parallel_for.h>
#include <tbb/blocked_range.h>

// Static member initialization
std::unique_ptr<tbb::global_control> ThreadPoolManager::global_control_;
bool ThreadPoolManager::initialized_ = false;

void ThreadPoolManager::initialize(size_t num_threads)
{
    if (!initialized_)
    {
        global_control_ = std::make_unique<tbb::global_control>(
            tbb::global_control::max_allowed_parallelism, num_threads);
        initialized_ = true;
        Logger::info("Thread pool manager initialized with " + std::to_string(num_threads) + " threads");
    }
}

void ThreadPoolManager::shutdown()
{
    if (initialized_)
    {
        global_control_.reset();
        initialized_ = false;
        Logger::info("Thread pool manager shutdown");
    }
}

void ThreadPoolManager::processFilesAsync(
    const std::string &db_path,
    const std::vector<std::string> &files,
    std::function<void()> on_complete)
{
    if (!initialized_)
    {
        Logger::error("Thread pool manager not initialized");
        return;
    }

    if (files.empty())
    {
        if (on_complete)
            on_complete();
        return;
    }

    Logger::info("Processing " + std::to_string(files.size()) + " files asynchronously");

    // Use TBB parallel_for to process files in parallel
    tbb::parallel_for(tbb::blocked_range<size_t>(0, files.size()),
                      [&](const tbb::blocked_range<size_t> &range)
                      {
                          for (size_t i = range.begin(); i != range.end(); ++i)
                          {
                              processFileWithOwnConnection(db_path, files[i]);
                          }
                      });

    if (on_complete)
    {
        on_complete();
    }
}

void ThreadPoolManager::processFileAsync(
    const std::string &db_path,
    const std::string &file_path,
    std::function<void()> on_complete)
{
    if (!initialized_)
    {
        Logger::error("Thread pool manager not initialized");
        return;
    }

    // Use TBB to process a single file asynchronously
    tbb::parallel_for(tbb::blocked_range<size_t>(0, 1),
                      [&](const tbb::blocked_range<size_t> &range)
                      {
                          processFileWithOwnConnection(db_path, file_path);
                      });

    if (on_complete)
    {
        on_complete();
    }
}

void ThreadPoolManager::processFileWithOwnConnection(const std::string &db_path, const std::string &file_path)
{
    try
    {
        // Each thread gets its own DatabaseManager (and thus its own SQLite connection)
        DatabaseManager local_db(db_path);
        MediaProcessingOrchestrator orchestrator(local_db);

        Logger::info("Processing file with thread-local database connection: " + file_path);

        // Process the file using the orchestrator
        auto processing_observable = orchestrator.processAllScannedFiles(1); // Single thread per file

        processing_observable.subscribe(
            [](const FileProcessingEvent &event)
            {
                if (event.success)
                {
                    Logger::info("Thread-local processed file: " + event.file_path +
                                 " (format: " + event.artifact_format +
                                 ", confidence: " + std::to_string(event.artifact_confidence) + ")");
                }
                else
                {
                    Logger::warn("Thread-local processing failed for: " + event.file_path +
                                 " - " + event.error_message);
                }
            },
            [](const std::exception &e)
            {
                Logger::error("Thread-local processing error: " + std::string(e.what()));
            },
            []()
            {
                Logger::info("Thread-local processing completed");
            });
    }
    catch (const std::exception &e)
    {
        Logger::error("Failed to process file with thread-local connection: " + file_path +
                      " - " + std::string(e.what()));
    }
}