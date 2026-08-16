#pragma once

#include <string>
#include <memory>

namespace MediaDedup
{
    class DatabaseManager;
    class UnifiedObservableConfigManager;

    /**
     * @brief Outcome of processing one image.
     *
     * Ignored is NOT a flavour of Succeeded. Process() applies the
     * resolution filter itself and marks the file ignored (processed = 3)
     * when it trips; the caller must then leave that state alone. While this
     * was a bool, "ignored" returned true, the caller read that as success and
     * overwrote processed with 2 one line later — so every file the resolution
     * filter ignored was silently un-ignored, kept appearing in the day view,
     * and sat forever with no artifact and nothing owed to it. That was all
     * 1,005 such files on the reference library (100% of them).
     */
    enum class ImageProcessOutcome
    {
        Failed,    ///< Processing failed; caller records the error state.
        Succeeded, ///< Artifacts written; caller marks processed = 2.
        Ignored    ///< Deliberately skipped and ALREADY marked; caller must not touch the state.
    };

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
         * @return Succeeded, Failed, or Ignored — see ImageProcessOutcome. An
         *         Ignored file has already had its state written here; the
         *         caller must not overwrite it.
         */
        ImageProcessOutcome Process(const std::string &processing_file_path, const std::string &original_file_path, DatabaseManager &db, std::shared_ptr<UnifiedObservableConfigManager> config_manager);

    private:
        // Configuration for processing
        static constexpr int DEFAULT_INPUT_SIZE = 224;
        static constexpr int DEFAULT_EMBEDDING_DIM = 512;
    };
}
