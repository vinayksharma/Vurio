#pragma once

#include <filesystem>
#include <string>
#include <functional>
#include <vector>
#include <memory>

namespace fs = std::filesystem;

// Simple custom observable implementation
template <typename T>
class SimpleObservable
{
public:
    using Observer = std::function<void(const T &)>;
    using ErrorHandler = std::function<void(const std::exception &)>;
    using CompleteHandler = std::function<void()>;

    SimpleObservable(std::function<void(Observer, ErrorHandler, CompleteHandler)> source)
        : source_(std::move(source)) {}

    void subscribe(Observer onNext, ErrorHandler onError = nullptr, CompleteHandler onComplete = nullptr)
    {
        if (source_)
        {
            source_(onNext, onError, onComplete);
        }
    }

    void subscribe(Observer onNext, CompleteHandler onComplete)
    {
        subscribe(onNext, nullptr, onComplete);
    }

    void subscribe(Observer onNext)
    {
        subscribe(onNext, nullptr, nullptr);
    }

private:
    std::function<void(Observer, ErrorHandler, CompleteHandler)> source_;
};

class FileUtils
{
public:
    /**
     * Lists all files in a directory as a simple observable stream
     * @param dir_path The directory path to scan
     * @param recursive Whether to scan subdirectories recursively
     * @return Observable stream of fully qualified file paths
     */
    static SimpleObservable<std::string> listFilesAsObservable(const std::string &dir_path, bool recursive);

    /**
     * @brief Compute the SHA-256 hash of a file's contents
     * @param file_path Path to the file
     * @return Hex string of the SHA-256 hash, or empty string on error
     */
    static std::string computeFileHash(const std::string &file_path);

private:
    /**
     * Internal implementation of file listing with error handling
     * @param dir_path The directory path to scan
     * @param recursive Whether to scan subdirectories recursively
     * @return Observable stream of fully qualified file paths
     */
    static SimpleObservable<std::string> listFilesInternal(const std::string &dir_path, bool recursive);

    /**
     * Validates if the given path exists and is a directory
     * @param path The path to validate
     * @return True if path exists and is a directory, false otherwise
     */
    static bool isValidDirectory(const std::string &path);

    /**
     * Recursively scans a directory with error handling for permission issues
     * @param dir_path The directory path to scan
     * @param onNext Callback function for each file found
     */
    static void scanDirectoryRecursively(const std::string &dir_path,
                                         std::function<void(const std::string &)> onNext);
};