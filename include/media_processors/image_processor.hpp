#pragma once

#include <string>
#include <memory>

namespace MediaDedup
{
    class DatabaseManager;
    class UnifiedObservableConfigManager;

    /**
     * @brief Image processor that handles embedding-based duplicate detection
     *
     * This class uses CLIP deep learning embeddings for high-quality image processing.
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
         * @brief Process an image file using CLIP embeddings
         *
         * This method uses CLIP deep learning model for semantic image understanding.
         *
         * @param processing_file_path Path to the file to process (may be transcoded file in cache)
         * @param original_file_path Original source file path for metadata storage
         * @param db Database manager for storing image artifacts
         * @param config_manager Configuration manager for dynamic config loading
         * @return true if processing was successful
         */
        bool Process(const std::string &processing_file_path, const std::string &original_file_path, DatabaseManager &db, std::shared_ptr<UnifiedObservableConfigManager> config_manager);

    private:
        // Configuration for processing
        static constexpr int DEFAULT_INPUT_SIZE = 224;
        static constexpr int DEFAULT_EMBEDDING_DIM = 512;
    };
}
