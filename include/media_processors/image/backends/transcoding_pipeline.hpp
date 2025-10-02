#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <memory>

namespace MediaDedup
{
    class UnifiedObservableConfigManager;
    /**
     * @brief Configuration for transcoding operations
     */
    struct TranscodingConfig
    {
        bool enabled = true;
        int timeout_ms = 60000;
        bool preserve_metadata = true;
    };

    /**
     * @brief Pipeline class for handling image transcoding operations
     *
     * This class provides a high-level interface for transcoding raw image files
     * to a format suitable for processing by OpenCV and other backends.
     */
    class TranscodingPipeline
    {
    public:
        /**
         * @brief Transcode a raw image file to TIFF format in memory
         *
         * @param file_path The path to the raw image file
         * @param tiff_data Output vector to store the transcoded TIFF data
         * @param config Configuration for transcoding operation
         * @return true if transcoding was successful, false otherwise
         */
        static bool TranscodeToMemory(const std::string &file_path,
                                      std::vector<std::uint8_t> &tiff_data,
                                      const TranscodingConfig &config = TranscodingConfig{});

        /**
         * @brief Transcode a raw image file to TIFF format and save to disk
         *
         * @param source_file_path The path to the raw image file
         * @param transcoded_file_path Output path where the transcoded file will be saved
         * @param config Configuration for transcoding operation
         * @return true if transcoding was successful, false otherwise
         */
        static bool TranscodeToFile(const std::string &source_file_path,
                                    std::string &transcoded_file_path,
                                    const TranscodingConfig &config = TranscodingConfig{});

        /**
         * @brief Generate a unique filename with UUID suffix
         *
         * @param original_path The original file path
         * @param extension The new file extension (e.g., ".tiff")
         * @return Unique filename with UUID suffix
         */
        static std::string GenerateUniqueFilename(const std::string &original_path, const std::string &extension);

        /**
         * @brief Check if a file needs transcoding based on its extension
         *
         * @param file_path The path to the file to check
         * @return true if the file needs transcoding, false otherwise
         */
        static bool NeedsTranscoding(const std::string &file_path);

        /**
         * @brief Get default transcoding configuration
         *
         * @return Default configuration for transcoding operations
         */
        static TranscodingConfig GetDefaultConfig();

        /**
         * @brief Get transcoding configuration from config manager
         *
         * @param config_manager Configuration manager instance
         * @return Configuration loaded from config manager with defaults as fallback
         */
        static TranscodingConfig GetConfigFromManager(std::shared_ptr<UnifiedObservableConfigManager> config_manager);
    };
}
