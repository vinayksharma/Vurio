#include "core/file_utils.hpp"
#include <stdexcept>
#include <iostream>
#include "logging/logger.hpp"
#include <openssl/sha.h>
#include <fstream>
#include <vector>
#include <sstream>
#include <iomanip>

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
                        // Non-recursive directory iteration
                        for (const auto &entry : fs::directory_iterator(dir_path))
                        {
                            if (entry.is_regular_file())
                            {
                                onNext(entry.path().string());
                            }
                        }
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
    // Custom recursive scanner that handles permission errors gracefully
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
}

std::string FileUtils::computeFileHash(const std::string &file_path)
{
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