#pragma once

#include <string>
#include <functional>
#include <chrono>

namespace MediaDedup
{
    struct ImageTaskConfig
    {
        int timeout_ms = 30000;
        bool retry_enabled = true;
        int retry_max_attempts = 2; // total attempts = 1 + retries
        int retry_base_delay_ms = 500;
    };

    enum class ImageTaskResult
    {
        Success,
        Timeout,
        PermanentFailure
    };

    class ImageTask
    {
    public:
        using Work = std::function<bool()>; // returns true on success

        static ImageTaskResult Run(const std::string &file_path,
                                   const std::string &mode_label,
                                   const ImageTaskConfig &cfg,
                                   const Work &work);
    };
}












