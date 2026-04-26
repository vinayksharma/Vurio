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
         * @brief Transcode a raw image file to JPEG format in memory
         *
         * @param file_path The path to the raw image file
         * @param jpeg_data Output vector to store the transcoded JPEG data
         * @param config Configuration for transcoding operation
         * @return true if transcoding was successful, false otherwise
         */
        static bool TranscodeToMemory(const std::string &file_path,
                                      std::vector<std::uint8_t> &jpeg_data,
                                      const TranscodingConfig &config = TranscodingConfig{});

        /**
         * @brief Transcode a raw image file to JPEG format and save to disk
         *
         * @param source_file_path The path to the raw image file
         * @param transcoded_file_path Output path where the transcoded file will be saved
         * @param config Configuration for transcoding operation
         * @param base_directory Base directory where the transcoded file should be created (empty for current directory)
         * @return true if transcoding was successful, false otherwise
         */
        static bool TranscodeToFile(const std::string &source_file_path,
                                    std::string &transcoded_file_path,
                                    const TranscodingConfig &config = TranscodingConfig{},
                                    const std::string &base_directory = "");

        /**
         * @brief Generate a unique filename with UUID suffix
         *
         * @param original_path The original file path
         * @param extension The new file extension (e.g., ".jpg")
         * @param base_directory The base directory where the file should be created (empty for current directory)
         * @return Unique full path with UUID suffix
         */
        static std::string GenerateUniqueFilename(const std::string &original_path, const std::string &extension, const std::string &base_directory = "");

        /**
         * @brief Check if a file needs transcoding based on its extension (RAW only)
         *
         * @param file_path The path to the file to check
         * @return true if the file needs transcoding, false otherwise
         */
        static bool NeedsTranscoding(const std::string &file_path);

        /**
         * @brief Check if a file should get a cached QHD preview (either it's RAW or it's a large image)
         * 
         * @param file_path The path to the file to check
         * @param config_manager Config manager to read threshold
         * @return true if the file should get a QHD preview cache
         */
        static bool NeedsQhdPreviewCache(const std::string &file_path, std::shared_ptr<UnifiedObservableConfigManager> config_manager);

        /**
         * @brief Check if a non-RAW file should be transcoded to JPEG for embedding.
         * Uses config media.images.<extension>.transcode (e.g. media.images.bmp.transcode).
         * When true, ImageMagick converts to JPEG before the ONNX pipeline so OpenCV reads reliably.
         *
         * @param file_path The path to the file to check
         * @param config_manager Config manager to read media.images.<ext>.transcode (nullable: if null, returns false)
         * @return true if the file should be transcoded to JPEG before embedding
         */
        static bool NeedsTranscodingForEmbedding(const std::string &file_path,
                                                 std::shared_ptr<UnifiedObservableConfigManager> config_manager);

        /**
         * @brief Transcode a non-RAW image file to JPEG and save to disk (no RAW validation).
         * Use for formats like BMP that OpenCV may not read reliably.
         *
         * @param source_file_path Path to the image file
         * @param transcoded_file_path Output path for the JPEG file
         * @param base_directory Directory for the output file
         * @return true if successful
         */
        static bool TranscodeNonRawToFile(const std::string &source_file_path,
                                          std::string &transcoded_file_path,
                                          const std::string &base_directory = "");

        /**
         * @brief Validate that a transcoded file can be read by OpenCV
         *
         * @param file_path The path to the transcoded file to validate
         * @return true if the file is valid and readable by OpenCV, false otherwise
         */
        static bool ValidateTranscodedFile(const std::string &file_path);

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
