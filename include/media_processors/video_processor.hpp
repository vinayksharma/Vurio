#pragma once

#include <string>

namespace MediaDedup
{
    /**
     * @brief Video processor that handles different processing modes for video files
     *
     * This class provides three processing modes:
     * - ProcessFast: Fast processing with minimal quality impact
     * - ProcessBalanced: Balanced processing between speed and quality
     * - ProcessQuality: High-quality processing with longer processing time
     */
    class VideoProcessor
    {
    public:
        /**
         * @brief Constructor
         */
        VideoProcessor() = default;

        /**
         * @brief Destructor
         */
        ~VideoProcessor() = default;

        /**
         * @brief Process a video file using fast processing mode
         *
         * This method is designed for quick processing with minimal quality impact.
         * Currently returns true as a placeholder implementation.
         *
         * @param file_path Fully qualified path to the video file
         * @return true if processing was initiated successfully
         */
        bool ProcessFast(const std::string &file_path);

        /**
         * @brief Process a video file using balanced processing mode
         *
         * This method provides a balance between processing speed and quality.
         * Currently returns true as a placeholder implementation.
         *
         * @param file_path Fully qualified path to the video file
         * @return true if processing was initiated successfully
         */
        bool ProcessBalanced(const std::string &file_path);

        /**
         * @brief Process a video file using quality processing mode
         *
         * This method prioritizes quality over processing speed.
         * Currently returns true as a placeholder implementation.
         *
         * @param file_path Fully qualified path to the video file
         * @return true if processing was initiated successfully
         */
        bool ProcessQuality(const std::string &file_path);

    private:
        // Future implementation will include actual video processing logic
        // For now, these are placeholder methods that return true
    };
}
