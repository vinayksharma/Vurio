#pragma once

#include <string>

namespace MediaDedup
{
    /**
     * @brief Video processor for duplicate detection (placeholder)
     *
     * This class provides video processing for duplicate detection.
     * Current implementation is a placeholder for future video processing logic.
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
         * @brief Process a video file
         *
         * Placeholder implementation for video processing.
         * Future implementation will include actual video processing logic.
         *
         * @param file_path Fully qualified path to the video file
         * @return true if processing was initiated successfully
         */
        bool Process(const std::string &file_path);

    private:
        // Future implementation will include actual video processing logic
    };
}
