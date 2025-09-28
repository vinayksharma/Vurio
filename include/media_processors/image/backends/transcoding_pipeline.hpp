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
        std::string quality = "high"; // high, medium, low
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

    private:
        /**
         * @brief Apply quality settings to transcoding configuration
         *
         * @param config Configuration to modify based on quality setting
         */
        static void ApplyQualitySettings(TranscodingConfig &config);
    };
}
