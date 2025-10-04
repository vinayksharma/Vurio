#pragma once

#include <string>
#include <memory>

namespace MediaDedup
{
    class DatabaseManager;
    class UnifiedObservableConfigManager;

    /**
     * @brief Image processor that handles different processing modes for image files
     *
     * This class provides three processing modes:
     * - ProcessFast: Fast processing with minimal quality impact using OpenCV perceptual hashing
     * - ProcessBalanced: Balanced processing between speed and quality using feature detection
     * - ProcessQuality: High-quality processing with longer processing time using ONNX models
     */
    class ImageProcessor
    {
    public:
        /**
         * @brief Constructor
         */
        ImageProcessor() = default;

        /**
         * @brief Destructor
         */
        ~ImageProcessor() = default;

        /**
         * @brief Process an image file using fast processing mode
         *
         * This method uses OpenCV perceptual hashing for quick processing with minimal quality impact.
         *
         * @param processing_file_path Path to the file to process (may be transcoded file in cache)
         * @param original_file_path Original source file path for metadata storage
         * @param db Database manager for storing image artifacts
         * @param config_manager Configuration manager for dynamic config loading
         * @return true if processing was successful
         */
        bool ProcessFast(const std::string &processing_file_path, const std::string &original_file_path, DatabaseManager &db, std::shared_ptr<UnifiedObservableConfigManager> config_manager);

        /**
         * @brief Process an image file using balanced processing mode
         *
         * This method uses feature detection for balanced processing between speed and quality.
         *
         * @param processing_file_path Path to the file to process (may be transcoded file in cache)
         * @param original_file_path Original source file path for metadata storage
         * @param db Database manager for storing image artifacts
         * @param config_manager Configuration manager for dynamic config loading
         * @return true if processing was successful
         */
        bool ProcessBalanced(const std::string &processing_file_path, const std::string &original_file_path, DatabaseManager &db, std::shared_ptr<UnifiedObservableConfigManager> config_manager);

        /**
         * @brief Process an image file using quality processing mode
         *
         * This method uses ONNX models for high-quality processing with longer processing time.
         *
         * @param processing_file_path Path to the file to process (may be transcoded file in cache)
         * @param original_file_path Original source file path for metadata storage
         * @param db Database manager for storing image artifacts
         * @param config_manager Configuration manager for dynamic config loading
         * @return true if processing was successful
         */
        bool ProcessQuality(const std::string &processing_file_path, const std::string &original_file_path, DatabaseManager &db, std::shared_ptr<UnifiedObservableConfigManager> config_manager);

    private:
        /**
         * @brief Create a stub hash for raw files that fail transcoding
         * @param file_path Path to the raw file
         * @param db Database manager for storing the stub hash
         * @return true if stub hash was created successfully
         */
        bool createStubHashForRawFile(const std::string &file_path, DatabaseManager &db);
        // Configuration for different processing modes
        static constexpr int DEFAULT_THUMB_SIZE = 256;
        static constexpr int DEFAULT_RESIZE_LONG_EDGE = 1024;
        static constexpr int DEFAULT_MAX_KEYPOINTS = 1000;
        static constexpr int DEFAULT_INPUT_SIZE = 224;
        static constexpr int DEFAULT_EMBEDDING_DIM = 512;
    };
}
