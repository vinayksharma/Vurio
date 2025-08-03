#include "core/file_scanner.hpp"
#include "database/database_manager.hpp"
#include "core/media_processor.hpp"
#include "core/file_utils.hpp"
#include "core/transcoding_manager.hpp"
#include "logging/logger.hpp"

FileScanner::FileScanner(const std::string &db_path)
    : files_scanned_(0), files_stored_(0), files_skipped_(0)
{
    db_manager_ = &DatabaseManager::getInstance(db_path);
    Logger::info("FileScanner initialized with database: " + db_path);
}

size_t FileScanner::scanDirectory(const std::string &dir_path, bool recursive)
{
    Logger::info("Starting directory scan: " + dir_path + " (recursive: " + (recursive ? "yes" : "no") + ")");

    // Clear previous stats
    clearStats();

    try
    {
        // Subscribe to file stream
        auto file_stream = FileUtils::listFilesAsObservable(dir_path, recursive);

        file_stream.subscribe(
            [this](const std::string &file_path)
            {
                this->handleFile(file_path);
            },
            [this](const std::exception &error)
            {
                Logger::error("Scan error: " + std::string(error.what()));
            },
            [this]()
            {
                Logger::info("Directory scan completed. Scanned: " + std::to_string(files_scanned_) +
                             ", Stored: " + std::to_string(files_stored_) +
                             ", Skipped: " + std::to_string(files_skipped_));
            });
    }
    catch (const std::exception &e)
    {
        Logger::error("Error during directory scanning: " + std::string(e.what()));
    }

    return files_stored_;
}

bool FileScanner::scanFile(const std::string &file_path)
{
    Logger::debug("Scanning single file: " + file_path);

    files_scanned_++;

    // Check if file is supported before storing
    if (!MediaProcessor::isSupportedFile(file_path))
    {
        Logger::debug("Skipping unsupported file during scan: " + file_path);
        files_skipped_++;
        return false;
    }

    // Store only supported files in the database (skip metadata computation during scanning for performance)
    DBOpResult scan_result = db_manager_->storeScannedFile(file_path);
    if (!scan_result.success)
    {
        Logger::error("Failed to store file in scanned_files: " + file_path + ". DB error: " + scan_result.error_message);
        return false;
    }

    // Check if this is a raw file that needs transcoding
    if (TranscodingManager::isRawFile(file_path))
    {
        Logger::info("Detected raw file for transcoding: " + file_path);
        TranscodingManager::getInstance().queueForTranscoding(file_path);
    }

    files_stored_++;
    Logger::debug("Stored supported file during scan: " + file_path);
    return true;
}

void FileScanner::clearStats()
{
    files_scanned_ = 0;
    files_stored_ = 0;
    files_skipped_ = 0;
}

void FileScanner::handleFile(const std::string &file_path)
{
    Logger::debug("Handling file during scan: " + file_path);

    files_scanned_++;

    // Check if file is supported before storing
    if (!MediaProcessor::isSupportedFile(file_path))
    {
        Logger::debug("Skipping unsupported file during scan: " + file_path);
        files_skipped_++;
        return;
    }

    // Store only supported files in the database (skip metadata computation during scanning for performance)
    DBOpResult scan_result = db_manager_->storeScannedFile(file_path);
    if (!scan_result.success)
    {
        Logger::error("Failed to store file in scanned_files: " + file_path + ". DB error: " + scan_result.error_message);
        return;
    }

    // Check if this is a raw file that needs transcoding
    if (TranscodingManager::isRawFile(file_path))
    {
        Logger::info("Detected raw file for transcoding: " + file_path);
        TranscodingManager::getInstance().queueForTranscoding(file_path);
    }

    files_stored_++;
    Logger::debug("Stored supported file during scan: " + file_path);
}