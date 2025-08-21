#pragma once

#include <tbb/parallel_for.h>
#include <tbb/blocked_range.h>
#include <tbb/global_control.h>
#include <memory>
#include <functional>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include "database/database_manager.hpp"
#include "media_processing_orchestrator.hpp"
#include "core/server_config_manager.hpp"
#include "logging/logger.hpp"

// Forward declarations
class FileProcessingEvent;
class ProcessingResult;
class DBOpResult;
enum class DedupMode;

/**
 * @brief Thread pool manager for async file processing with contained database connections
 * and dynamic thread pool resizing capabilities
 *
 * This class manages a TBB thread pool and ensures each thread gets its own
 * DatabaseManager instance to avoid SQLite concurrency issues.
 * Supports dynamic thread count changes and configuration hot-reloading.
 */
class ThreadPoolManager : public ConfigObserver
{
public:
    /**
     * @brief Initialize the thread pool manager
     * @param num_threads Number of threads in the pool (default: 4)
     */
    static void initialize(size_t num_threads);

    /**
     * @brief Shutdown the thread pool manager
     */
    static void shutdown();

    /**
     * @brief Dynamically resize the thread pool
     * @param new_num_threads New number of threads
     * @return true if resize was successful, false otherwise
     */
    static bool resizeThreadPool(size_t new_num_threads);

    /**
     * @brief Get current thread pool size
     * @return Current number of threads in the pool
     */
    static size_t getCurrentThreadCount();

    /**
     * @brief Get maximum allowed thread count
     * @return Maximum thread count from configuration
     */
    static size_t getMaxAllowedThreadCount();

    /**
     * @brief Process files asynchronously using TBB
     * @param db_path Database path
     * @param files Vector of file paths to process
     * @param on_complete Completion callback
     */
    static void processFilesAsync(
        const std::string &db_path,
        const std::vector<std::string> &files,
        std::function<void()> on_complete);

    /**
     * @brief Process a single file asynchronously using TBB
     * @param db_path Database path
     * @param file_path File path to process
     * @param on_complete Completion callback
     */
    static void processFileAsync(
        const std::string &db_path,
        const std::string &file_path,
        std::function<void()> on_complete);

    /**
     * @brief Process all scanned files asynchronously using TBB with dynamic configuration
     * @param max_threads Maximum number of threads to use (can be overridden by config)
     * @param on_event Event callback for each processed file
     * @param on_error Error callback for fatal errors
     * @param on_complete Completion callback
     */
    static void processAllScannedFilesAsync(
        int max_threads,
        std::function<void(const FileProcessingEvent &)> on_event,
        std::function<void(const std::exception &)> on_error,
        std::function<void()> on_complete);

    /**
     * @brief Process a file with its own database connection
     * @param db_path Database path
     * @param file_path File path to process
     */
    static void processFileWithOwnConnection(const std::string &db_path, const std::string &file_path);

    /**
     * @brief Configuration change handler for dynamic updates
     * @param event Configuration change event
     */
    void onConfigChanged(const ConfigEvent &event) override;

    // FIXED: Made constructor public so it can be used with std::make_unique
    ThreadPoolManager() = default;

private:
    // Static member variables
    static std::unique_ptr<tbb::global_control> global_control_;
    static std::atomic<bool> initialized_;
    static std::atomic<size_t> current_thread_count_;
    static std::mutex resize_mutex_;

    // Helper methods
    static void updateThreadPoolSize(size_t new_size);
    static bool validateThreadCount(size_t thread_count);
};