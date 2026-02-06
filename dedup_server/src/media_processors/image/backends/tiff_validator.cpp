#include "media_processors/image/backends/tiff_validator.hpp"
#include <tiffio.h>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <Poco/Logger.h>

namespace MediaDedup
{
    // Custom TIFF error handler to suppress libtiff warnings/errors to stderr
    static void tiffSilentErrorHandler(const char *, const char *, va_list)
    {
        // Suppress error output - we'll handle errors through return values
    }

    static void tiffSilentWarningHandler(const char *, const char *, va_list)
    {
        // Suppress warning output
    }

    TiffValidator::ValidationResult TiffValidator::validate(const std::string &file_path)
    {
        Poco::Logger &logger = Poco::Logger::get("TiffValidator");
        ValidationResult result;
        result.is_valid = false;

        // Check if file exists
        if (!std::filesystem::exists(file_path))
        {
            result.error_code = -1;
            result.error_message = "File does not exist";
            logger.debug("TIFF validation failed: %s - %s", file_path, result.error_message);
            return result;
        }

        // Check if file has TIFF extension
        if (!isTiffFile(file_path))
        {
            result.error_code = -2;
            result.error_message = "Not a TIFF file (by extension)";
            logger.debug("TIFF validation skipped: %s - %s", file_path, result.error_message);
            return result;
        }

        // Quick check: validate TIFF magic bytes before using libtiff
        if (!checkMagicBytes(file_path))
        {
            result.error_code = -3;
            result.error_message = "Invalid TIFF magic bytes (corrupted or not a TIFF file)";
            logger.warning("TIFF validation failed: %s - %s", file_path, result.error_message);
            return result;
        }

        // Set custom error handlers to suppress libtiff stderr output
        TIFFErrorHandler oldErrorHandler = TIFFSetErrorHandler(tiffSilentErrorHandler);
        TIFFErrorHandler oldWarningHandler = TIFFSetWarningHandler(tiffSilentWarningHandler);

        // Try to open with libtiff for full validation
        TIFF *tif = TIFFOpen(file_path.c_str(), "r");
        
        if (tif == nullptr)
        {
            // Restore original handlers
            TIFFSetErrorHandler(oldErrorHandler);
            TIFFSetWarningHandler(oldWarningHandler);

            result.error_code = -3;
            result.error_message = "libtiff failed to open file (corrupted TIFF structure)";
            logger.warning("TIFF validation failed: %s - %s", file_path, result.error_message);
            return result;
        }

        // Successfully opened - validate basic TIFF properties
        bool has_width = false;
        bool has_height = false;
        uint32_t width = 0, height = 0;

        // Check for required TIFF tags
        if (TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &width) == 1)
        {
            has_width = true;
        }

        if (TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &height) == 1)
        {
            has_height = true;
        }

        TIFFClose(tif);

        // Restore original handlers
        TIFFSetErrorHandler(oldErrorHandler);
        TIFFSetWarningHandler(oldWarningHandler);

        // Validate that we got essential properties
        if (!has_width || !has_height || width == 0 || height == 0)
        {
            result.error_code = -3;
            result.error_message = "TIFF file missing required tags or has zero dimensions";
            logger.warning("TIFF validation failed: %s - %s (width: %u, height: %u)", 
                          file_path, result.error_message, width, height);
            return result;
        }

        // File is valid
        result.is_valid = true;
        result.error_code = 0;
        result.error_message = "Valid TIFF file";
        logger.debug("TIFF validation passed: %s (width: %u, height: %u)", file_path, width, height);

        return result;
    }

    bool TiffValidator::isTiffFile(const std::string &file_path)
    {
        std::filesystem::path path(file_path);
        std::string ext = path.extension().string();

        // Convert to lowercase
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        return (ext == ".tif" || ext == ".tiff");
    }

    bool TiffValidator::checkMagicBytes(const std::string &file_path)
    {
        std::ifstream file(file_path, std::ios::binary);
        if (!file.is_open())
        {
            return false;
        }

        // Read first 4 bytes
        unsigned char magic[4];
        file.read(reinterpret_cast<char *>(magic), 4);

        if (file.gcount() < 4)
        {
            return false;
        }

        // Check for TIFF magic bytes:
        // Little-endian: 0x49 0x49 0x2A 0x00 (II*\0)
        // Big-endian:    0x4D 0x4D 0x00 0x2A (MM\0*)
        bool is_little_endian = (magic[0] == 0x49 && magic[1] == 0x49 && magic[2] == 0x2A && magic[3] == 0x00);
        bool is_big_endian = (magic[0] == 0x4D && magic[1] == 0x4D && magic[2] == 0x00 && magic[3] == 0x2A);

        return is_little_endian || is_big_endian;
    }

} // namespace MediaDedup

