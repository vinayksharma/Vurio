#include "core/file_processor.hpp"
#include "database/database_manager.hpp"
#include "core/media_processor.hpp"
#include "core/server_config_manager.hpp"
#include "logging/logger.hpp"
#include <iostream>

FileProcessor::FileProcessor(const std::string &db_path)
    : total_files_processed_(0), successful_files_processed_(0)
{
    db_manager_ = &DatabaseManager::getInstance(db_path);
    Logger::info("FileProcessor initialized with database: " + db_path);
}

FileProcessor::~FileProcessor()
{
    if (db_manager_)
    {
        db_manager_->waitForWrites();
    }
}

size_t FileProcessor::processDirectory(const std::string &dir_path, bool recursive)
{
    Logger::info("Starting directory processing: " + dir_path + " (recursive: " + (recursive ? "yes" : "no") + ")");

    // Clear previous stats
    clearStats();

    try
    {
        // Get current quality mode from configuration
        auto &config_manager = ServerConfigManager::getInstance();
        DedupMode current_mode = config_manager.getDedupMode();

        Logger::info("Using quality mode: " + DedupModes::getModeName(current_mode));

        // Subscribe to file stream
        auto file_stream = FileUtils::listFilesAsObservable(dir_path, recursive);

        file_stream.subscribe(
            [this](const std::string &file_path)
            {
                this->handleFile(file_path);
            },
            [this](const std::exception &error)
            {
                this->handleError(error);
            },
            [this]()
            {
                this->handleComplete();
            });

        Logger::info("Directory processing completed. Processed " +
                     std::to_string(total_files_processed_) + " files, " +
                     std::to_string(successful_files_processed_) + " successful");
    }
    catch (const std::exception &e)
    {
        Logger::error("Error during directory processing: " + std::string(e.what()));
    }

    return total_files_processed_;
}

FileProcessResult FileProcessor::processFile(const std::string &file_path)
{
    Logger::info("Processing single file: " + file_path);
    try
    {
        if (!MediaProcessor::isSupportedFile(file_path))
        {
            std::string msg = "Unsupported file type: " + file_path;
            Logger::warn(msg);
            return FileProcessResult(false, msg);
        }

        // First, ensure the file is stored in scanned_files table
        DBOpResult scan_result = db_manager_->storeScannedFile(file_path);
        if (!scan_result.success)
        {
            std::string msg = "Failed to store file in scanned_files: " + file_path + ". DB error: " + scan_result.error_message;
            Logger::error(msg);
            return FileProcessResult(false, msg);
        }

        auto &config_manager = ServerConfigManager::getInstance();
        DedupMode current_mode = config_manager.getDedupMode();
        ProcessingResult result = MediaProcessor::processFile(file_path, current_mode);
        DBOpResult db_result = db_manager_->storeProcessingResult(file_path, current_mode, result);
        if (!db_result.success)
        {
            std::string msg = "Failed to store processing result for: " + file_path + ". DB error: " + db_result.error_message;
            Logger::error(msg);
            return FileProcessResult(false, msg);
        }
        total_files_processed_++;
        if (result.success)
        {
            successful_files_processed_++;
            Logger::info("Successfully processed: " + file_path);
            return FileProcessResult(true);
        }
        else
        {
            std::string msg = "Failed to process: " + file_path + " - " + result.error_message;
            Logger::warn(msg);
            return FileProcessResult(false, msg);
        }
    }
    catch (const std::exception &e)
    {
        std::string msg = "Error processing file " + file_path + ": " + std::string(e.what());
        Logger::error(msg);
        return FileProcessResult(false, msg);
    }
}

std::pair<size_t, size_t> FileProcessor::getProcessingStats() const
{
    return {total_files_processed_, successful_files_processed_};
}

void FileProcessor::clearStats()
{
    total_files_processed_ = 0;
    successful_files_processed_ = 0;
}

void FileProcessor::waitForWrites()
{
    if (db_manager_)
    {
        db_manager_->waitForWrites();
    }
}

std::string FileProcessor::getFileCategory(const std::string &file_path)
{
    if (MediaProcessor::isImageFile(file_path))
        return "Image";
    if (MediaProcessor::isVideoFile(file_path))
        return "Video";
    if (MediaProcessor::isAudioFile(file_path))
        return "Audio";
    return "Unknown";
}

void FileProcessor::handleFile(const std::string &file_path)
{
    Logger::debug("Handling file: " + file_path);
    try
    {
        if (!MediaProcessor::isSupportedFile(file_path))
        {
            Logger::debug("Skipping unsupported file: " + file_path);
            return;
        }

        // First, ensure the file is stored in scanned_files table
        DBOpResult scan_result = db_manager_->storeScannedFile(file_path);
        if (!scan_result.success)
        {
            Logger::error("Failed to store file in scanned_files: " + file_path + ". DB error: " + scan_result.error_message);
            return;
        }

        auto &config_manager = ServerConfigManager::getInstance();
        DedupMode current_mode = config_manager.getDedupMode();
        ProcessingResult result = MediaProcessor::processFile(file_path, current_mode);
        DBOpResult db_result = db_manager_->storeProcessingResult(file_path, current_mode, result);
        if (!db_result.success)
        {
            Logger::error("Failed to store processing result for: " + file_path + ". DB error: " + db_result.error_message);
            return;
        }
        total_files_processed_++;
        if (result.success)
        {
            successful_files_processed_++;
            Logger::info("Successfully processed: " + file_path +
                         " (format: " + result.artifact.format +
                         ", confidence: " + std::to_string(result.artifact.confidence) + ")");
        }
        else
        {
            Logger::warn("Failed to process: " + file_path + " - " + result.error_message);
        }
    }
    catch (const std::exception &e)
    {
        Logger::error("Error handling file " + file_path + ": " + std::string(e.what()));
    }
}

void FileProcessor::handleError(const std::exception &error)
{
    Logger::error("File processing error: " + std::string(error.what()));
}

void FileProcessor::handleComplete()
{
    Logger::info("File processing completed. Total: " + std::to_string(total_files_processed_) +
                 ", Successful: " + std::to_string(successful_files_processed_));
}