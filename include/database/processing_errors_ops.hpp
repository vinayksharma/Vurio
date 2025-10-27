#pragma once

#include <string>
#include "config/config_enums.hpp"

namespace MediaDedup
{
    class DatabaseManager;

    struct ProcessingErrorRow
    {
        int id = 0;
        std::string file_path;
        std::string server_mode;
        int error_code = 0;
        std::string error_message;
        std::string error_source;
        std::string timestamp;
    };

    class ProcessingErrorsOps
    {
    public:
        /**
         * @brief Ensure the processing_errors table exists in the database
         * @param db Database manager instance
         * @return true if table exists or was created successfully
         */
        static bool ensureTable(DatabaseManager &db);

        /**
         * @brief Insert a processing error record
         * @param db Database manager instance
         * @param file_path Path to the file that failed processing
         * @param mode Server mode during processing (EMBEDDING)
         * @param error_code Error code (e.g., -1, -3, -4, -101, etc.)
         * @param error_message Detailed error message from exception
         * @param error_source Source of error (e.g., "ImageMagick", "OpenCV", "ONNX")
         * @return true if insert was successful
         */
        static bool insertError(
            DatabaseManager &db,
            const std::string &file_path,
            ServerMode mode,
            int error_code,
            const std::string &error_message,
            const std::string &error_source = "");
    };
}
