#pragma once

#include <string>

namespace MediaDedup
{
    /**
     * @brief TIFF file validation using libtiff
     * 
     * Validates TIFF file integrity before processing to avoid OpenCV failures.
     * Uses libtiff library to check TIFF header, IFD structure, and basic validity.
     */
    class TiffValidator
    {
    public:
        /**
         * @brief Validation result structure
         */
        struct ValidationResult
        {
            bool is_valid = false;
            std::string error_message;
            int error_code = 0; // 0: valid, -1: file not found, -2: not TIFF, -3: corrupted
        };

        /**
         * @brief Validate a TIFF file
         * @param file_path Path to the TIFF file
         * @return ValidationResult with status and error details
         */
        static ValidationResult validate(const std::string &file_path);

        /**
         * @brief Check if file is a TIFF by extension
         * @param file_path Path to check
         * @return true if file has .tif or .tiff extension
         */
        static bool isTiffFile(const std::string &file_path);

    private:
        /**
         * @brief Check TIFF magic bytes without opening with libtiff
         * @param file_path Path to check
         * @return true if file starts with TIFF magic bytes (II or MM)
         */
        static bool checkMagicBytes(const std::string &file_path);
    };

} // namespace MediaDedup

