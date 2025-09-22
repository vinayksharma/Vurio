#include "media_processors/image_processor.hpp"
#include <Poco/Logger.h>

namespace MediaDedup
{
    bool ImageProcessor::ProcessFast(const std::string &file_path)
    {
        // Placeholder implementation - returns true silently
        // Future implementation will include actual fast image processing logic
        Poco::Logger::get("ImageProcessor").debug("Processing image in FAST mode: " + file_path);
        return true;
    }

    bool ImageProcessor::ProcessBalanced(const std::string &file_path)
    {
        // Placeholder implementation - returns true silently
        // Future implementation will include actual balanced image processing logic
        Poco::Logger::get("ImageProcessor").debug("Processing image in BALANCED mode: " + file_path);
        return true;
    }

    bool ImageProcessor::ProcessQuality(const std::string &file_path)
    {
        // Placeholder implementation - returns true silently
        // Future implementation will include actual quality image processing logic
        Poco::Logger::get("ImageProcessor").debug("Processing image in QUALITY mode: " + file_path);
        return true;
    }
}
