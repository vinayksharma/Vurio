#pragma once

#include "database/database_manager.hpp"
#include "core/media_processor.hpp"
#include "core/file_utils.hpp"
#include "logging/logger.hpp"
#include <string>
#include <functional>

class FileScanner
{
public:
    FileScanner(const std::string &db_path);
    ~FileScanner() = default;

    // Scan a directory and store only supported files in the database
    size_t scanDirectory(const std::string &dir_path, bool recursive = false);

    // Scan a single file and store it if supported
    bool scanFile(const std::string &file_path);

    // Get scan statistics
    size_t getFilesScanned() const { return files_scanned_; }
    size_t getFilesStored() const { return files_stored_; }
    size_t getFilesSkipped() const { return files_skipped_; }

    // Clear statistics
    void clearStats();

private:
    DatabaseManager *db_manager_;
    size_t files_scanned_;
    size_t files_stored_;
    size_t files_skipped_;

    // Handle individual file during scanning
    void handleFile(const std::string &file_path);
};