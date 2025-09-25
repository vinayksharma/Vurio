#pragma once

#include <string>

namespace MediaDedup
{
    /**
     * @brief Audio processor that handles different processing modes for audio files
     *
     * This class provides three processing modes:
     * - ProcessFast: Fast processing with minimal quality impact
     * - ProcessBalanced: Balanced processing between speed and quality
     * - ProcessQuality: High-quality processing with longer processing time
     */
    class AudioProcessor
    {
    public:
        /**
         * @brief Constructor
         */
        AudioProcessor() = default;

        /**
         * @brief Destructor
         */
        ~AudioProcessor() = default;

        /**
         * @brief Process an audio file using fast processing mode
         *
         * This method is designed for quick processing with minimal quality impact.
         * Currently returns true as a placeholder implementation.
         *
         * @param file_path Fully qualified path to the audio file
         * @return true if processing was initiated successfully
         */
        bool ProcessFast(const std::string &file_path);

        /**
         * @brief Process an audio file using balanced processing mode
         *
         * This method provides a balance between processing speed and quality.
         * Currently returns true as a placeholder implementation.
         *
         * @param file_path Fully qualified path to the audio file
         * @return true if processing was initiated successfully
         */
        bool ProcessBalanced(const std::string &file_path);

        /**
         * @brief Process an audio file using quality processing mode
         *
         * This method prioritizes quality over processing speed.
         * Currently returns true as a placeholder implementation.
         *
         * @param file_path Fully qualified path to the audio file
         * @return true if processing was initiated successfully
         */
        bool ProcessQuality(const std::string &file_path);

    private:
        // Future implementation will include actual audio processing logic
        // For now, these are placeholder methods that return true
    };
}
