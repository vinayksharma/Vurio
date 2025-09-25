#include "media_processors/video_processor.hpp"
#include <Poco/Logger.h>

namespace MediaDedup
{
    bool VideoProcessor::ProcessFast(const std::string &file_path)
    {
        // Placeholder implementation - returns true silently
        // Future implementation will include actual fast video processing logic
        Poco::Logger::get("VideoProcessor").debug("Processing video in FAST mode: " + file_path);
        return true;
    }

    bool VideoProcessor::ProcessBalanced(const std::string &file_path)
    {
        // Placeholder implementation - returns true silently
        // Future implementation will include actual balanced video processing logic
        Poco::Logger::get("VideoProcessor").debug("Processing video in BALANCED mode: " + file_path);
        return true;
    }

    bool VideoProcessor::ProcessQuality(const std::string &file_path)
    {
        // Placeholder implementation - returns true silently
        // Future implementation will include actual quality video processing logic
        Poco::Logger::get("VideoProcessor").debug("Processing video in QUALITY mode: " + file_path);
        return true;
    }
}
