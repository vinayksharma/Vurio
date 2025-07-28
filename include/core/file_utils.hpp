#pragma once

#include <filesystem>
#include <string>
#include <functional>
#include <vector>
#include <memory>
#include <optional>
#include <chrono>

namespace fs = std::filesystem;

// Forward declarations
template <typename T>
class SimpleObservable;

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

/**
 * @brief File metadata for efficient change detection
 */
struct FileMetadata
{
    std::string file_path;
    std::time_t modification_time; // Last modification time
    std::time_t creation_time;     // Creation time
    uint64_t file_size;            // File size in bytes
    uint32_t inode;                // Inode number (for hard link detection)
    uint32_t device_id;            // Device ID (for mount point changes)

    // Comparison operators for change detection
    bool operator==(const FileMetadata &other) const;
    bool operator!=(const FileMetadata &other) const;

    // Convert to string for logging/debugging
    std::string toString() const;
};

/**
 * @brief File utilities for efficient file operations
 */
class FileUtils
{
public:
    /**
     * @brief Get file metadata efficiently (no file content reading)
     * @param file_path Path to the file
     * @return Optional FileMetadata if file exists and is accessible
     */
    static std::optional<FileMetadata> getFileMetadata(const std::string &file_path);

    /**
     * @brief Check if file has changed based on metadata comparison
     * @param file_path Path to the file
     * @param stored_metadata Previously stored metadata
     * @return True if file has changed, false if unchanged
     */
    static bool hasFileChanged(const std::string &file_path, const FileMetadata &stored_metadata);

    /**
     * @brief Get file metadata as a compact string for database storage
     * @param metadata File metadata
     * @return Compact string representation
     */
    static std::string metadataToString(const FileMetadata &metadata);

    /**
     * @brief Parse file metadata from string
     * @param metadata_str String representation of metadata
     * @return Optional FileMetadata if parsing successful
     */
    static std::optional<FileMetadata> metadataFromString(const std::string &metadata_str);

    /**
     * Lists all files in a directory as a simple observable stream
     * @param dir_path Directory path to scan
     * @param recursive Whether to scan recursively
     * @return SimpleObservable that emits file paths
     */
    static SimpleObservable<std::string> listFilesAsObservable(const std::string &dir_path, bool recursive = false);

    /**
     * Scans a directory recursively and calls the provided function for each file
     * @param dir_path Directory path to scan
     * @param onNext Function to call for each file found
     */
    static void scanDirectoryRecursively(const std::string &dir_path, std::function<void(const std::string &)> onNext);

    /**
     * Validates if a path is a valid directory
     * @param path Path to validate
     * @return true if path is a valid directory, false otherwise
     */
    static bool isValidDirectory(const std::string &path);

    /**
     * Computes SHA256 hash of a file
     * @param file_path Path to the file
     * @return SHA256 hash as hexadecimal string
     */
    static std::string computeFileHash(const std::string &file_path);

private:
    static SimpleObservable<std::string> listFilesInternal(const std::string &dir_path, bool recursive);
};