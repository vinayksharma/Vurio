#pragma once
#include <functional>
#include <filesystem>
#include "filesmanager/file_record.hpp"
#include "filesmanager/file_scanner_options.hpp"

namespace MediaDedup::Files
{
    using FileCallback = std::function<void(const FileRecord &)>;

    void scan(const std::filesystem::path &directory,
              const FileScannerOptions &options,
              const FileCallback &onFile);

    void scanDirectoryRecursive(const std::filesystem::path &directory,
                               const FileScannerOptions &options,
                               const FileCallback &onFile);

    inline void scan(const std::filesystem::path &directory,
                     const FileCallback &onFile)
    {
        scan(directory, FileScannerOptions{}, onFile);
    }

    bool isModified(const FileRecord &previous, const FileRecord &current);
    ChangeKind classifyChange(const FileRecord &previous, const FileRecord &current);
}
