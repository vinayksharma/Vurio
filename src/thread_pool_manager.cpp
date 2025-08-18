#include "core/thread_pool_manager.hpp"
#include "database/database_manager.hpp"
#include "core/media_processor.hpp"
#include "core/file_utils.hpp"
#include "core/transcoding_manager.hpp"
#include "core/duplicate_linker.hpp"
#include "core/server_config_manager.hpp"
#include "logging/logger.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <tbb/parallel_for.h>
#include <tbb/blocked_range.h>

// Static member initialization
std::unique_ptr<tbb::global_control> ThreadPoolManager::global_control_;
std::atomic<bool> ThreadPoolManager::initialized_{false};
std::atomic<size_t> ThreadPoolManager::current_thread_count_{0};
std::mutex ThreadPoolManager::resize_mutex_;

// Singleton instance for ConfigObserver registration
static ThreadPoolManager *instance_ptr = nullptr;

void ThreadPoolManager::initialize(size_t num_threads)
{
    if (!initialized_.load())
    {
        std::lock_guard<std::mutex> lock(resize_mutex_);
        if (!initialized_.load()) // Double-check pattern
        {
            if (validateThreadCount(num_threads))
            {
                global_control_ = std::make_unique<tbb::global_control>(
                    tbb::global_control::max_allowed_parallelism, num_threads);
                current_thread_count_.store(num_threads);
                initialized_.store(true);

                // Create singleton instance for ConfigObserver registration
                if (!instance_ptr)
                {
                    instance_ptr = new ThreadPoolManager();
                }

                // Register as config observer for dynamic updates
                ServerConfigManager::getInstance().addObserver(instance_ptr);

                Logger::info("Thread pool manager initialized with " + std::to_string(num_threads) + " threads");
            }
            else
            {
                Logger::error("Invalid thread count: " + std::to_string(num_threads) + ". Using default: 4");
                global_control_ = std::make_unique<tbb::global_control>(
                    tbb::global_control::max_allowed_parallelism, 4);
                current_thread_count_.store(4);
                initialized_.store(true);

                // Create singleton instance for ConfigObserver registration
                if (!instance_ptr)
                {
                    instance_ptr = new ThreadPoolManager();
                }

                // Register as config observer for dynamic updates
                ServerConfigManager::getInstance().addObserver(instance_ptr);

                Logger::info("Thread pool manager initialized with default 4 threads");
            }
        }
    }
}

void ThreadPoolManager::shutdown()
{
    if (initialized_.load())
    {
        std::lock_guard<std::mutex> lock(resize_mutex_);
        if (initialized_.load()) // Double-check pattern
        {
            // Unregister as config observer
            try
            {
                if (instance_ptr)
                {
                    ServerConfigManager::getInstance().removeObserver(instance_ptr);
                    delete instance_ptr;
                    instance_ptr = nullptr;
                }
            }
            catch (...)
            {
                // Ignore errors during shutdown
            }

            global_control_.reset();
            current_thread_count_.store(0);
            initialized_.store(false);
            Logger::info("Thread pool manager shutdown");
        }
    }
}

bool ThreadPoolManager::resizeThreadPool(size_t new_num_threads)
{
    if (!initialized_.load())
    {
        Logger::error("Cannot resize thread pool - not initialized");
        return false;
    }

    if (!validateThreadCount(new_num_threads))
    {
        Logger::error("Invalid thread count for resize: " + std::to_string(new_num_threads));
        return false;
    }

    std::lock_guard<std::mutex> lock(resize_mutex_);

    if (new_num_threads == current_thread_count_.load())
    {
        Logger::debug("Thread pool already at requested size: " + std::to_string(new_num_threads));
        return true;
    }

    try
    {
        Logger::info("Resizing thread pool from " + std::to_string(current_thread_count_.load()) +
                     " to " + std::to_string(new_num_threads) + " threads");

        // Create new global control with new thread count
        global_control_.reset();
        global_control_ = std::make_unique<tbb::global_control>(
            tbb::global_control::max_allowed_parallelism, new_num_threads);

        current_thread_count_.store(new_num_threads);

        Logger::info("Thread pool successfully resized to " + std::to_string(new_num_threads) + " threads");
        return true;
    }
    catch (const std::exception &e)
    {
        Logger::error("Failed to resize thread pool: " + std::string(e.what()));
        return false;
    }
}

size_t ThreadPoolManager::getCurrentThreadCount()
{
    return current_thread_count_.load();
}

size_t ThreadPoolManager::getMaxAllowedThreadCount()
{
    try
    {
        auto &config_manager = ServerConfigManager::getInstance();
        return static_cast<size_t>(config_manager.getMaxProcessingThreads());
    }
    catch (...)
    {
        Logger::warn("Could not get max allowed thread count from config, returning current count");
        return current_thread_count_.load();
    }
}

void ThreadPoolManager::onConfigChanged(const ConfigEvent &event)
{
    if (event.type == ConfigEventType::GENERAL_CONFIG_CHANGED ||
        event.key == "max_processing_threads")
    {
        try
        {
            auto &config_manager = ServerConfigManager::getInstance();
            size_t new_thread_count = static_cast<size_t>(config_manager.getMaxProcessingThreads());

            Logger::info("Configuration change detected - max_processing_threads: " + std::to_string(new_thread_count));

            if (resizeThreadPool(new_thread_count))
            {
                Logger::info("Thread pool successfully resized due to configuration change");
            }
            else
            {
                Logger::warn("Failed to resize thread pool due to configuration change");
            }
        }
        catch (const std::exception &e)
        {
            Logger::error("Error handling thread count configuration change: " + std::string(e.what()));
        }
    }
}

bool ThreadPoolManager::validateThreadCount(size_t thread_count)
{
    // Minimum: 1 thread (for basic functionality)
    // Maximum: 64 threads (reasonable upper limit)
    if (thread_count < 1 || thread_count > 64)
    {
        Logger::warn("Thread count " + std::to_string(thread_count) + " is outside valid range [1-64]");
        return false;
    }

    // Check system capabilities (basic check)
    size_t max_hardware_threads = std::thread::hardware_concurrency();
    if (max_hardware_threads > 0 && thread_count > max_hardware_threads * 2)
    {
        Logger::warn("Thread count " + std::to_string(thread_count) +
                     " exceeds 2x hardware concurrency (" + std::to_string(max_hardware_threads) +
                     "). This may cause performance degradation.");
        // Don't fail, just warn
    }

    return true;
}

void ThreadPoolManager::updateThreadPoolSize(size_t new_size)
{
    resizeThreadPool(new_size);
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
                              // Use the singleton DatabaseManager for all threads
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

void ThreadPoolManager::processAllScannedFilesAsync(
    int max_threads,
    std::function<void(const FileProcessingEvent &)> on_event,
    std::function<void(const std::exception &)> on_error,
    std::function<void()> on_complete)
{
    if (!initialized_.load())
    {
        Logger::error("Thread pool manager not initialized");
        if (on_error)
            on_error(std::runtime_error("Thread pool manager not initialized"));
        return;
    }

    try
    {
        // Get database manager instance
        DatabaseManager &dbMan = DatabaseManager::getInstance();
        auto &config_manager = ServerConfigManager::getInstance();

        // Use dynamic thread count from configuration if available, otherwise use passed parameter
        size_t actual_thread_count = getMaxAllowedThreadCount();
        if (actual_thread_count == 0)
        {
            actual_thread_count = static_cast<size_t>(max_threads);
        }

        Logger::info("Starting TBB-based file processing through ThreadPoolManager with " +
                     std::to_string(actual_thread_count) + " threads (configured: " +
                     std::to_string(max_threads) + ")");

        // Get current configuration for processing (this may change during processing)
        DedupMode current_mode = config_manager.getDedupMode();
        bool pre_process_quality_stack = config_manager.getPreProcessQualityStack();
        int batch_size = config_manager.getProcessingBatchSize();

        // Log current processing configuration
        Logger::info("Processing configuration - Mode: " + DedupModes::getModeName(current_mode) +
                     ", PreProcessQualityStack: " + (pre_process_quality_stack ? "enabled" : "disabled") +
                     ", BatchSize: " + std::to_string(batch_size));

        // Determine which modes to process based on current configuration
        std::vector<DedupMode> modes_to_process;
        if (pre_process_quality_stack)
        {
            modes_to_process = {DedupMode::FAST, DedupMode::BALANCED, DedupMode::QUALITY};
            Logger::info("PreProcessQualityStack enabled - processing all quality levels (FAST, BALANCED, QUALITY)");
        }
        else
        {
            modes_to_process = {current_mode};
            Logger::info("PreProcessQualityStack disabled - processing only selected mode: " + DedupModes::getModeName(current_mode));
        }

        // Get files that need processing
        std::vector<std::pair<std::string, std::string>> files_to_process;
        if (pre_process_quality_stack)
        {
            files_to_process = dbMan.getAndMarkFilesForProcessingAnyMode(batch_size);
        }
        else
        {
            files_to_process = dbMan.getAndMarkFilesForProcessing(current_mode, batch_size);
        }

        if (files_to_process.empty())
        {
            Logger::info("No files need processing");
            if (on_complete)
                on_complete();
            return;
        }

        Logger::info("Starting TBB-based processing of " + std::to_string(files_to_process.size()) +
                     " files with " + std::to_string(actual_thread_count) + " threads");

        // Thread-safe counters for progress tracking
        std::atomic<size_t> processed_count{0};
        std::atomic<size_t> successful_processed{0};
        std::atomic<size_t> failed_processed{0};
        std::atomic<size_t> mode_changed_during_processing{0};

        // Use TBB parallel_for to process files in parallel
        tbb::parallel_for(tbb::blocked_range<size_t>(0, files_to_process.size()),
                          [&](const tbb::blocked_range<size_t> &range)
                          {
                              for (size_t i = range.begin(); i != range.end(); ++i)
                              {
                                  const std::string &file_path = files_to_process[i].first;

                                  // Create FileProcessingEvent for this file
                                  FileProcessingEvent event;
                                  event.file_path = file_path;

                                  try
                                  {
                                      // Check if file is supported
                                      if (!MediaProcessor::isSupportedFile(file_path))
                                      {
                                          event.success = false;
                                          event.error_message = "Unsupported file type: " + file_path;
                                          Logger::error(event.error_message);
                                          processed_count.fetch_add(1);
                                          failed_processed.fetch_add(1);
                                          if (on_event)
                                              on_event(event);
                                          continue;
                                      }

                                      // Process the file for each required mode
                                      bool any_success = false;
                                      std::string last_error;

                                      for (const auto &process_mode : modes_to_process)
                                      {
                                          // Check if processing mode has changed during processing
                                          DedupMode current_processing_mode = config_manager.getDedupMode();
                                          if (current_processing_mode != current_mode)
                                          {
                                              Logger::info("Processing mode changed from " + DedupModes::getModeName(current_mode) +
                                                           " to " + DedupModes::getModeName(current_processing_mode) +
                                                           " during processing. Continuing with original mode for consistency.");
                                              mode_changed_during_processing.fetch_add(1);
                                          }

                                          Logger::info("TBB thread processing file: " + file_path + " with mode: " + DedupModes::getModeName(process_mode));

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
                                              // Allow retry in a future cycle
                                              dbMan.setProcessingFlag(file_path, process_mode);
                                              failed_processed.fetch_add(1);
                                              continue;
                                          }

                                          // Process the file for this mode
                                          ProcessingResult result = MediaProcessor::processFile(actual_file_path, process_mode);

                                          // Store the processing result in the database
                                          DBOpResult db_result = dbMan.storeProcessingResult(file_path, process_mode, result);
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
                                              Logger::info("TBB thread successfully processed file: " + file_path +
                                                           " (format: " + result.artifact.format +
                                                           ", confidence: " + std::to_string(result.artifact.confidence) + ")");
                                              any_success = true;

                                              // Mark as successfully processed for this specific mode
                                              dbMan.setProcessingFlag(file_path, process_mode);

                                              // Update success counter
                                              successful_processed.fetch_add(1);

                                              // Notify duplicate linker that new results are available
                                              DuplicateLinker::getInstance().notifyNewResults();
                                          }
                                          else
                                          {
                                              Logger::warn("TBB thread failed to process file: " + file_path + " - " + result.error_message);
                                              last_error = result.error_message;

                                              // Mark as failed for this specific mode (set to 0 to allow retry)
                                              dbMan.setProcessingFlag(file_path, process_mode);

                                              // Update failure counter
                                              failed_processed.fetch_add(1);
                                          }

                                          // Update progress counter
                                          processed_count.fetch_add(1);
                                      }

                                      // Set final event status
                                      if (any_success)
                                      {
                                          event.success = true;
                                          event.artifact_format = "processed";
                                          event.artifact_confidence = 1.0;
                                      }
                                      else
                                      {
                                          event.success = false;
                                          event.error_message = last_error;
                                      }

                                      if (on_event)
                                          on_event(event);
                                  }
                                  catch (const std::exception &e)
                                  {
                                      Logger::error("TBB thread error processing file " + file_path + ": " + std::string(e.what()));
                                      event.success = false;
                                      event.error_message = "Exception: " + std::string(e.what());
                                      failed_processed.fetch_add(1);
                                      if (on_event)
                                          on_event(event);
                                  }
                              }
                          });

        // Log final statistics including mode change information
        Logger::info("TBB-based processing completed - Processed: " + std::to_string(processed_count.load()) +
                     ", Successful: " + std::to_string(successful_processed.load()) +
                     ", Failed: " + std::to_string(failed_processed.load()) +
                     ", Mode changes detected: " + std::to_string(mode_changed_during_processing.load()));

        if (on_complete)
            on_complete();
    }
    catch (const std::exception &e)
    {
        Logger::error("Failed to start TBB-based file processing: " + std::string(e.what()));
        if (on_error)
            on_error(e);
    }
}

void ThreadPoolManager::processFileWithOwnConnection(const std::string &db_path, const std::string &file_path)
{
    try
    {
        // Get database manager instance
        DatabaseManager &dbMan = DatabaseManager::getInstance();
        MediaProcessingOrchestrator orchestrator(dbMan);

        Logger::info("Processing file with shared database connection: " + file_path);

        // Process the file using the orchestrator
        auto &config_manager = ServerConfigManager::getInstance();
        auto processing_observable = orchestrator.processAllScannedFiles(config_manager.getMaxProcessingThreads());

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
        Logger::error("Failed to process file with shared connection: " + file_path +
                      " - " + std::string(e.what()));
    }
}