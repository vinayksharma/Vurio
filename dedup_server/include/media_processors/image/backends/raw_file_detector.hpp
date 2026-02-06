#pragma once

#include <string>
#include <vector>

namespace MediaDedup
{
    /**
     * @brief Utility class for detecting raw image files that require transcoding
     *
     * This class provides static methods to determine if a file is a raw image format
     * that needs to be transcoded before processing by OpenCV or other backends.
     */
    class RawFileDetector
    {
    public:
        /**
         * @brief Check if a file is a raw image format that requires transcoding
         *
         * @param file_path The fully qualified path to the file to check
         * @return true if the file is a raw format that needs transcoding, false otherwise
         */
        static bool IsRawFile(const std::string &file_path);

        /**
         * @brief Get the file extension from a file path
         *
         * @param file_path The file path to extract extension from
         * @return The file extension in lowercase, or empty string if no extension
         */
        static std::string GetFileExtension(const std::string &file_path);

        /**
         * @brief Get list of all supported raw file extensions
         *
         * @return Vector of raw file extensions (without dots)
         */
        static std::vector<std::string> GetSupportedRawExtensions();

    private:
        /**
         * @brief Check if an extension is a raw format
         *
         * @param extension The file extension to check (should be lowercase)
         * @return true if the extension is a raw format, false otherwise
         */
        static bool IsRawExtension(const std::string &extension);
    };
}
