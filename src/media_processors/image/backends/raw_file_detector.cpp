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
        std::string filename = path.filename().string();

        // Handle double extensions like .tif.cr2, .jpg.raw, etc.
        // Look for the last occurrence of a known raw extension
        std::vector<std::string> raw_extensions = GetSupportedRawExtensions();

        // Convert filename to lowercase for comparison
        std::string lower_filename = filename;
        std::transform(lower_filename.begin(), lower_filename.end(), lower_filename.begin(), ::tolower);

        // Check for raw extensions in order of preference (longer extensions first)
        std::sort(raw_extensions.begin(), raw_extensions.end(), [](const std::string &a, const std::string &b)
                  { return a.length() > b.length(); });

        for (const std::string &ext : raw_extensions)
        {
            std::string dot_ext = "." + ext;
            std::string lower_dot_ext = dot_ext;
            std::transform(lower_dot_ext.begin(), lower_dot_ext.end(), lower_dot_ext.begin(), ::tolower);

            if (lower_filename.length() >= lower_dot_ext.length() &&
                lower_filename.substr(lower_filename.length() - lower_dot_ext.length()) == lower_dot_ext)
            {
                return ext;
            }
        }

        // Fallback to standard extension detection
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
