#pragma once

#include <tbb/parallel_for.h>
#include <tbb/blocked_range.h>
#include <tbb/global_control.h>
#include <memory>
#include <functional>
#include <string>
#include "database_manager.hpp"
#include "media_processing_orchestrator.hpp"
#include "logging/logger.hpp"

/**
 * @brief Thread pool manager for async file processing with contained database connections
 *
 * This class manages a TBB thread pool and ensures each thread gets its own
 * DatabaseManager instance to avoid SQLite concurrency issues.
 */
class ThreadPoolManager
{
public:
    /**
     * @brief Initialize the thread pool manager
     * @param num_threads Number of threads in the pool (default: 4)
     */
    static void initialize(size_t num_threads = 4);

    /**
     * @brief Shutdown the thread pool manager
     */
    static void shutdown();

    /**
     * @brief Process files asynchronously using the thread pool
     * @param db_path Database path for each thread's connection
     * @param files Vector of file paths to process
     * @param on_complete Callback when all processing is complete
     */
    static void processFilesAsync(
        const std::string &db_path,
        const std::vector<std::string> &files,
        std::function<void()> on_complete = nullptr);

    /**
     * @brief Process a single file asynchronously
     * @param db_path Database path for the thread's connection
     * @param file_path File to process
     * @param on_complete Callback when processing is complete
     */
    static void processFileAsync(
        const std::string &db_path,
        const std::string &file_path,
        std::function<void()> on_complete = nullptr);

private:
    static std::unique_ptr<tbb::global_control> global_control_;
    static bool initialized_;

    /**
     * @brief Process a single file with its own database connection
     * @param db_path Database path
     * @param file_path File to process
     */
    static void processFileWithOwnConnection(const std::string &db_path, const std::string &file_path);
};