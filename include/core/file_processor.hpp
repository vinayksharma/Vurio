#pragma once

#include <string>
#include <memory>
#include "file_utils.hpp"
#include "media_processor.hpp"
#include "database_manager.hpp"
#include "server_config_manager.hpp"

/**
 * @brief Result of a file processing operation
 */
struct FileProcessResult
{
    bool success;
    std::string error_message;
    FileProcessResult(bool s = true, const std::string &msg = "") : success(s), error_message(msg) {}
};

/**
 * @brief File processor that integrates file scanning, media processing, and database storage
 *
 * This class observes file names emitted by FileUtils::listFilesInternal,
 * processes them with MediaProcessor using the current quality setting,
 * and stores the results in a SQLite database.
 */
class FileProcessor
{
public:
    /**
     * @brief Constructor
     * @param db_path Path to SQLite database file
     */
    explicit FileProcessor(const std::string &db_path);

    /**
     * @brief Destructor
     */
    ~FileProcessor();

    /**
     * @brief Process all files in a directory with current quality settings
     * @param dir_path Directory path to scan
     * @param recursive Whether to scan recursively
     * @return Number of files processed
     */
    size_t processDirectory(const std::string &dir_path, bool recursive = true);

    /**
     * @brief Process a single file with current quality settings
     * @param file_path Path to file to process
     * @return FileProcessResult with success flag and error message
     */
    FileProcessResult processFile(const std::string &file_path);

    /**
     * @brief Get processing statistics
     * @return Pair of (total_files, successful_files)
     */
    std::pair<size_t, size_t> getProcessingStats() const;

    /**
     * @brief Clear processing statistics
     */
    void clearStats();

    /**
     * @brief Get the category of a file (Audio, Image, or Video)
     * @param file_path Path to the file
     * @return std::string category name
     */
    static std::string getFileCategory(const std::string &file_path);

    /**
     * @brief Wait for all pending database writes to complete
     */
    void waitForWrites();

private:
    std::unique_ptr<DatabaseManager> db_manager_;
    size_t total_files_processed_;
    size_t successful_files_processed_;

    /**
     * @brief Handle a single file from the file stream
     * @param file_path Path to the file
     */
    void handleFile(const std::string &file_path);

    /**
     * @brief Handle processing errors
     * @param error Exception that occurred
     */
    void handleError(const std::exception &error);

    /**
     * @brief Handle completion of file processing
     */
    void handleComplete();
};