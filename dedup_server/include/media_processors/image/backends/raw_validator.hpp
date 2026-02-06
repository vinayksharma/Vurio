#pragma once

#include <string>

namespace MediaDedup
{
    /**
     * @brief RAW file validation using LibRaw
     * 
     * Validates RAW camera files before ImageMagick transcoding.
     * Uses LibRaw to check if file can be opened and decoded without full processing.
     */
    class RawValidator
    {
    public:
        /**
         * @brief Validation result structure
         */
        struct ValidationResult
        {
            bool is_valid = false;
            std::string error_message;
            int error_code = 0; // 0: valid, -1: file not found, -2: not RAW, -3: unsupported format, -4: corrupted
            std::string format_name; // e.g., "Canon CR2", "Nikon NEF"
        };

        /**
         * @brief Validate a RAW file using LibRaw
         * @param file_path Path to the RAW file
         * @return ValidationResult with status and error details
         */
        static ValidationResult validate(const std::string &file_path);

        /**
         * @brief Check if file is a RAW file by extension
         * @param file_path Path to check
         * @return true if file has a known RAW extension
         */
        static bool isRawFile(const std::string &file_path);

    private:
        /**
         * @brief Get human-readable format name from LibRaw descriptor
         * @param descriptor LibRaw format descriptor string
         * @return Formatted camera/format name
         */
        static std::string formatDescriptor(const char *descriptor);
    };

} // namespace MediaDedup

