#include "core/file_utils.hpp"
#include <stdexcept>
#include <iostream>
#include "logging/logger.hpp"
#include <openssl/sha.h>
#include <fstream>
#include <vector>
#include <sstream>
#include <iomanip>

// macOS native APIs for faster file enumeration
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <dirent.h>
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;

SimpleObservable<std::string> FileUtils::listFilesAsObservable(const std::string &dir_path, bool recursive)
{
    return listFilesInternal(dir_path, recursive);
}

SimpleObservable<std::string> FileUtils::listFilesInternal(const std::string &dir_path, bool recursive)
{
    using Observer = std::function<void(const std::string &)>;
    using ErrorHandler = std::function<void(const std::exception &)>;
    using CompleteHandler = std::function<void()>;
    return SimpleObservable<std::string>(
        std::function<void(Observer, ErrorHandler, CompleteHandler)>(
            [dir_path, recursive](Observer onNext, ErrorHandler onError, CompleteHandler onComplete)
            {
                try
                {
                    // Validate directory exists
                    if (!isValidDirectory(dir_path))
                    {
                        std::string msg = "Invalid directory path: " + dir_path;
                        Logger::warn(msg);
                        if (onError)
                        {
                            onError(std::runtime_error(msg));
                        }
                        return;
                    }
                    if (recursive)
                    {
                        // Custom recursive directory iteration with error handling
                        scanDirectoryRecursively(dir_path, onNext);
                    }
                    else
                    {
#ifdef __APPLE__
                        // Use C-based directory enumeration for faster performance
                        DIR *dir = opendir(dir_path.c_str());
                        if (!dir)
                        {
                            Logger::warn("Could not access directory: " + dir_path);
                            return;
                        }

                        struct dirent *entry;
                        while ((entry = readdir(dir)) != nullptr)
                        {
                            // Skip . and ..
                            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                            {
                                continue;
                            }

                            std::string full_path = dir_path + "/" + entry->d_name;
                            struct stat st;

                            if (stat(full_path.c_str(), &st) == 0)
                            {
                                if (S_ISREG(st.st_mode))
                                {
                                    // Regular file
                                    onNext(full_path);
                                }
                            }
                        }
                        closedir(dir);
#else
                        // Non-recursive directory iteration
                        for (const auto &entry : fs::directory_iterator(dir_path))
                        {
                            if (entry.is_regular_file())
                            {
                                onNext(entry.path().string());
                            }
                        }
#endif
                    }
                    if (onComplete)
                    {
                        onComplete();
                    }
                }
                catch (const std::exception &e)
                {
                    std::string msg = "Error listing files in directory: " + dir_path + ": " + e.what();
                    Logger::warn(msg);
                    if (onError)
                    {
                        onError(std::runtime_error(msg));
                    }
                }
            }));
}

bool FileUtils::isValidDirectory(const std::string &path)
{
    try
    {
        fs::path dir_path(path);
        return fs::exists(dir_path) && fs::is_directory(dir_path);
    }
    catch (const std::exception &e)
    {
        return false;
    }
}

void FileUtils::scanDirectoryRecursively(const std::string &dir_path,
                                         std::function<void(const std::string &)> onNext)
{
#ifdef __APPLE__
    // Use C-based directory enumeration for faster performance
    DIR *dir = opendir(dir_path.c_str());
    if (!dir)
    {
        Logger::warn("Could not access directory: " + dir_path);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr)
    {
        // Skip . and ..
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
        {
            continue;
        }

        std::string full_path = dir_path + "/" + entry->d_name;
        struct stat st;

        if (stat(full_path.c_str(), &st) == 0)
        {
            if (S_ISREG(st.st_mode))
            {
                // Regular file
                onNext(full_path);
            }
            else if (S_ISDIR(st.st_mode))
            {
                // Directory - recursively scan
                scanDirectoryRecursively(full_path, onNext);
            }
        }
    }
    closedir(dir);
#else
    // Fallback to std::filesystem for non-macOS platforms
    std::function<void(const fs::path &)> scanDirectory = [&](const fs::path &current_path)
    {
        try
        {
            for (const auto &entry : fs::directory_iterator(current_path))
            {
                try
                {
                    if (entry.is_regular_file())
                    {
                        onNext(entry.path().string());
                    }
                    else if (entry.is_directory())
                    {
                        // Recursively scan subdirectories
                        scanDirectory(entry.path());
                    }
                }
                catch (const fs::filesystem_error &e)
                {
                    // Log the error but continue scanning
                    Logger::warn("Skipping entry due to permission error: " + entry.path().string() + " - " + e.what());
                    continue;
                }
            }
        }
        catch (const fs::filesystem_error &e)
        {
            // Log the error but don't stop the entire scan
            Logger::warn("Error accessing directory " + current_path.string() + ": " + e.what());
        }
        catch (const std::exception &e)
        {
            // Log any other errors
            Logger::warn("Unexpected error scanning directory " + current_path.string() + ": " + e.what());
        }
    };
    // Start the recursive scan
    scanDirectory(fs::path(dir_path));
#endif
}

std::string FileUtils::computeFileHash(const std::string &file_path)
{
    Logger::debug("Reading entire file for hash computation: " + file_path);
    constexpr size_t buffer_size = 8192;
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_CTX sha256;
    if (SHA256_Init(&sha256) != 1)
        return "";
    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open())
        return "";
    std::vector<char> buffer(buffer_size);
    while (file.good())
    {
        file.read(buffer.data(), buffer_size);
        std::streamsize bytes_read = file.gcount();
        if (bytes_read > 0)
        {
            if (SHA256_Update(&sha256, buffer.data(), bytes_read) != 1)
                return "";
        }
    }
    if (SHA256_Final(hash, &sha256) != 1)
        return "";
    std::stringstream ss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i)
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    return ss.str();
}

// FileMetadata implementation
bool FileMetadata::operator==(const FileMetadata &other) const
{
    return modification_time == other.modification_time &&
           creation_time == other.creation_time &&
           file_size == other.file_size &&
           inode == other.inode &&
           device_id == other.device_id;
}

bool FileMetadata::operator!=(const FileMetadata &other) const
{
    return !(*this == other);
}

std::string FileMetadata::toString() const
{
    std::stringstream ss;
    ss << "FileMetadata{"
       << "path='" << file_path << "', "
       << "mod_time=" << modification_time << ", "
       << "create_time=" << creation_time << ", "
       << "size=" << file_size << ", "
       << "inode=" << inode << ", "
       << "device=" << device_id << "}";
    return ss.str();
}

std::optional<FileMetadata> FileUtils::getFileMetadata(const std::string &file_path)
{
#ifdef __APPLE__
    struct stat st;
    if (stat(file_path.c_str(), &st) != 0)
    {
        return std::nullopt;
    }

    FileMetadata metadata;
    metadata.file_path = file_path;
    metadata.modification_time = st.st_mtime;
    metadata.creation_time = st.st_birthtime; // macOS specific
    metadata.file_size = static_cast<uint64_t>(st.st_size);
    metadata.inode = static_cast<uint32_t>(st.st_ino);
    metadata.device_id = static_cast<uint32_t>(st.st_dev);

    return metadata;
#else
    // For non-macOS systems, use std::filesystem
    try
    {
        fs::path path(file_path);
        if (!fs::exists(path) || !fs::is_regular_file(path))
        {
            return std::nullopt;
        }

        FileMetadata metadata;
        metadata.file_path = file_path;
        metadata.modification_time = fs::last_write_time(path).time_since_epoch().count();
        metadata.creation_time = metadata.modification_time; // Fallback for non-macOS
        metadata.file_size = fs::file_size(path);
        metadata.inode = 0;     // Not easily available on all systems
        metadata.device_id = 0; // Not easily available on all systems

        return metadata;
    }
    catch (const std::exception &e)
    {
        Logger::error("Error getting file metadata for " + file_path + ": " + e.what());
        return std::nullopt;
    }
#endif
}

bool FileUtils::hasFileChanged(const std::string &file_path, const FileMetadata &stored_metadata)
{
    auto current_metadata = getFileMetadata(file_path);
    if (!current_metadata)
    {
        // File doesn't exist or can't be accessed
        return true; // Consider it changed
    }

    return *current_metadata != stored_metadata;
}

std::string FileUtils::metadataToString(const FileMetadata &metadata)
{
    std::stringstream ss;
    ss << metadata.modification_time << "|"
       << metadata.creation_time << "|"
       << metadata.file_size << "|"
       << metadata.inode << "|"
       << metadata.device_id;
    return ss.str();
}

std::optional<FileMetadata> FileUtils::metadataFromString(const std::string &metadata_str)
{
    try
    {
        std::stringstream ss(metadata_str);
        std::string token;

        FileMetadata metadata;

        // Parse modification_time
        if (!std::getline(ss, token, '|'))
            return std::nullopt;
        metadata.modification_time = std::stoll(token);

        // Parse creation_time
        if (!std::getline(ss, token, '|'))
            return std::nullopt;
        metadata.creation_time = std::stoll(token);

        // Parse file_size
        if (!std::getline(ss, token, '|'))
            return std::nullopt;
        metadata.file_size = std::stoull(token);

        // Parse inode
        if (!std::getline(ss, token, '|'))
            return std::nullopt;
        metadata.inode = std::stoul(token);

        // Parse device_id
        if (!std::getline(ss, token, '|'))
            return std::nullopt;
        metadata.device_id = std::stoul(token);

        return metadata;
    }
    catch (const std::exception &e)
    {
        Logger::error("Error parsing metadata string: " + metadata_str + " - " + e.what());
        return std::nullopt;
    }
}