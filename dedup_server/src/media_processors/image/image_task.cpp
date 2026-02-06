#include "media_processors/image/image_task.hpp"
#include <Poco/Logger.h>
#include <future>
#include <thread>

namespace MediaDedup
{
    static void sleepBackoff(int base_ms, int attempt)
    {
        int delay = base_ms * (1 << (attempt - 1));
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    }

    ImageTaskResult ImageTask::Run(const std::string &file_path,
                                   const std::string &mode_label,
                                   const ImageTaskConfig &cfg,
                                   const Work &work)
    {
        Poco::Logger &log = Poco::Logger::get("ImageTask");
        const int max_attempts = cfg.retry_enabled ? (1 + cfg.retry_max_attempts) : 1;

        for (int attempt = 1; attempt <= max_attempts; ++attempt)
        {
            log.information("image_task start file=%s mode=%s attempt=%d", file_path, mode_label, attempt);

            bool ok = false;
            try
            {
                // Execute work synchronously within the calling thread (TPM worker)
                // to avoid orphaned detached threads during shutdown.
                ok = work();
            }
            catch (const std::exception &e)
            {
                log.error("image_task exception file=%s mode=%s attempt=%d error=%s", file_path, mode_label, attempt, e.what());
                ok = false;
            }
            catch (...)
            {
                log.error("image_task unknown_exception file=%s mode=%s attempt=%d", file_path, mode_label, attempt);
                ok = false;
            }

            if (ok)
            {
                log.information("image_task success file=%s mode=%s attempt=%d", file_path, mode_label, attempt);
                return ImageTaskResult::Success;
            }

            if (attempt < max_attempts && cfg.retry_enabled)
            {
                sleepBackoff(cfg.retry_base_delay_ms, attempt);
                continue;
            }

            log.error("image_task failure file=%s mode=%s attempts=%d", file_path, mode_label, attempt);
            return ImageTaskResult::PermanentFailure;
        }

        return ImageTaskResult::PermanentFailure;
    }
}
