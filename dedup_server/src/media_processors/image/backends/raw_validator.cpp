#include "media_processors/image/backends/raw_validator.hpp"
#include "media_processors/image/backends/raw_file_detector.hpp"
#include <libraw/libraw.h>
#include <filesystem>
#include <Poco/Logger.h>

namespace MediaDedup
{
    RawValidator::ValidationResult RawValidator::validate(const std::string &file_path)
    {
        Poco::Logger &logger = Poco::Logger::get("RawValidator");
        ValidationResult result;
        result.is_valid = false;

        // Check if file exists
        if (!std::filesystem::exists(file_path))
        {
            result.error_code = -1;
            result.error_message = "File does not exist";
            logger.debug("RAW validation failed: %s - %s", file_path, result.error_message);
            return result;
        }

        // Check if file has RAW extension
        if (!isRawFile(file_path))
        {
            result.error_code = -2;
            result.error_message = "Not a RAW file (by extension)";
            logger.debug("RAW validation skipped: %s - %s", file_path, result.error_message);
            return result;
        }

        // Check minimum file size (RAW files are typically > 1MB, corrupted files are often truncated)
        std::error_code ec;
        auto file_size = std::filesystem::file_size(file_path, ec);
        if (ec || file_size < 1024 * 100) // Less than 100KB is suspicious for a RAW file
        {
            result.error_code = -4;
            result.error_message = "File too small to be a valid RAW file (likely truncated/corrupted)";
            logger.warning("RAW validation failed: %s - %s (size: %zu bytes)", 
                          file_path, result.error_message, file_size);
            return result;
        }

        // Use LibRaw to validate the file
        LibRaw raw_processor;

        // Attempt to open the RAW file
        int ret = raw_processor.open_file(file_path.c_str());

        if (ret != LIBRAW_SUCCESS)
        {
            // Determine specific error
            const char *error_str = libraw_strerror(ret);
            
            if (ret == LIBRAW_FILE_UNSUPPORTED)
            {
                result.error_code = -3;
                result.error_message = std::string("Unsupported RAW format: ") + error_str;
            }
            else if (ret == LIBRAW_IO_ERROR)
            {
                result.error_code = -4;
                result.error_message = std::string("I/O error reading RAW file: ") + error_str;
            }
            else
            {
                result.error_code = -4;
                result.error_message = std::string("Failed to open RAW file: ") + error_str;
            }

            logger.warning("RAW validation failed: %s - %s", file_path, result.error_message);
            raw_processor.recycle();
            return result;
        }

        // Successfully opened - get format information
        libraw_image_sizes_t &sizes = raw_processor.imgdata.sizes;
        libraw_iparams_t &iparams = raw_processor.imgdata.idata;

        // Validate that we have reasonable dimensions
        if (sizes.raw_width == 0 || sizes.raw_height == 0)
        {
            result.error_code = -4;
            result.error_message = "RAW file has zero dimensions (corrupted)";
            logger.warning("RAW validation failed: %s - %s", file_path, result.error_message);
            raw_processor.recycle();
            return result;
        }

        // Get camera make and model for logging
        std::string camera_make = iparams.make[0] ? iparams.make : "Unknown";
        std::string camera_model = iparams.model[0] ? iparams.model : "Unknown";
        result.format_name = camera_make + " " + camera_model;

        // Clean up LibRaw resources
        raw_processor.recycle();

        // File is valid
        result.is_valid = true;
        result.error_code = 0;
        result.error_message = "Valid RAW file";
        logger.debug("RAW validation passed: %s (format: %s, dimensions: %ux%u)", 
                    file_path, result.format_name, sizes.raw_width, sizes.raw_height);

        return result;
    }

    bool RawValidator::isRawFile(const std::string &file_path)
    {
        // Use existing RawFileDetector
        return RawFileDetector::IsRawFile(file_path);
    }

    std::string RawValidator::formatDescriptor(const char *descriptor)
    {
        if (descriptor == nullptr || descriptor[0] == '\0')
        {
            return "Unknown RAW format";
        }
        return std::string(descriptor);
    }

} // namespace MediaDedup

