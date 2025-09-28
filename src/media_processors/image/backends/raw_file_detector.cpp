#include "media_processors/image/backends/raw_file_detector.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>

namespace MediaDedup
{
    bool RawFileDetector::IsRawFile(const std::string &file_path)
    {
        if (file_path.empty())
        {
            return false;
        }

        std::string extension = GetFileExtension(file_path);
        return IsRawExtension(extension);
    }

    std::string RawFileDetector::GetFileExtension(const std::string &file_path)
    {
        if (file_path.empty())
        {
            return "";
        }

        std::filesystem::path path(file_path);
        std::string extension = path.extension().string();

        // Remove the leading dot if present
        if (!extension.empty() && extension[0] == '.')
        {
            extension = extension.substr(1);
        }

        // Convert to lowercase
        std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);

        return extension;
    }

    std::vector<std::string> RawFileDetector::GetSupportedRawExtensions()
    {
        return {
            "3fr", "arw", "bay", "cr2", "dcr", "dng", "fff", "iiq", "kdc", "mef",
            "mos", "mrw", "nef", "nrw", "orf", "pef", "raf", "raw", "rw2", "rwl",
            "rwz", "srw"};
    }

    bool RawFileDetector::IsRawExtension(const std::string &extension)
    {
        if (extension.empty())
        {
            return false;
        }

        std::vector<std::string> raw_extensions = GetSupportedRawExtensions();
        return std::find(raw_extensions.begin(), raw_extensions.end(), extension) != raw_extensions.end();
    }
}
