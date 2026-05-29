#include "media_processors/image/backends/raw_file_detector.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace MediaDedup
{
    ImageFormat RawFileDetector::DetectFormatFromMagicBytes(const std::string &file_path)
    {
        if (file_path.empty()) return ImageFormat::UNKNOWN;

        std::ifstream file(file_path, std::ios::binary);
        if (!file.is_open()) return ImageFormat::UNKNOWN;

        unsigned char header[16] = {0};
        file.read(reinterpret_cast<char*>(header), sizeof(header));
        std::streamsize bytes_read = file.gcount();

        if (bytes_read >= 3 && header[0] == 0xFF && header[1] == 0xD8 && header[2] == 0xFF)
        {
            return ImageFormat::JPEG;
        }
        
        if (bytes_read >= 8 && header[0] == 0x89 && header[1] == 0x50 && header[2] == 0x4E && header[3] == 0x47 &&
            header[4] == 0x0D && header[5] == 0x0A && header[6] == 0x1A && header[7] == 0x0A)
        {
            return ImageFormat::PNG;
        }

        if (bytes_read >= 4 && ((header[0] == 0x49 && header[1] == 0x49 && header[2] == 0x2A && header[3] == 0x00) ||
                                (header[0] == 0x4D && header[1] == 0x4D && header[2] == 0x00 && header[3] == 0x2A)))
        {
            return ImageFormat::TIFF;
        }

        if (bytes_read >= 12 && header[4] == 'f' && header[5] == 't' && header[6] == 'y' && header[7] == 'p')
        {
            std::string subtype(reinterpret_cast<char*>(&header[8]), 4);
            if (subtype == "heic" || subtype == "heix" || subtype == "hevc" || subtype == "mif1" || subtype == "msf1")
            {
                return ImageFormat::HEIC;
            }
        }

        if (bytes_read >= 6 && header[0] == 'G' && header[1] == 'I' && header[2] == 'F' && header[3] == '8')
        {
            return ImageFormat::GIF;
        }

        return ImageFormat::UNKNOWN;
    }

    bool RawFileDetector::IsRawFile(const std::string &file_path)
    {
        if (file_path.empty())
        {
            return false;
        }

        ImageFormat format = DetectFormatFromMagicBytes(file_path);
        // If we positively identify it as a standard non-RAW image, it's not a RAW file 
        // (even if its extension says otherwise)
        if (format == ImageFormat::JPEG || format == ImageFormat::PNG || format == ImageFormat::HEIC || format == ImageFormat::GIF)
        {
            return false;
        }

        // TIFF files or UNKNOWN headers could be RAW files (CR2, DNG, etc use TIFF headers).
        // Fall back to extension checking for these.
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
