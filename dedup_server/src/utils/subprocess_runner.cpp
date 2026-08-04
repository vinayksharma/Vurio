#include "utils/subprocess_runner.hpp"

#include <Poco/Logger.h>
#include <Poco/Process.h>
#include <chrono>
#include <thread>

namespace MediaDedup
{
    SubprocessResult runProcessWithTimeout(const std::string &command,
                                           const std::vector<std::string> &args,
                                           int timeout_ms,
                                           const std::map<std::string, std::string> &extra_env)
    {
        SubprocessResult result;
        Poco::Logger &logger = Poco::Logger::get("SubprocessRunner");

        Poco::ProcessHandle handle =
            extra_env.empty()
                ? Poco::Process::launch(command, args, nullptr, nullptr, nullptr)
                : Poco::Process::launch(command, args, nullptr, nullptr, nullptr, extra_env);

        if (timeout_ms <= 0)
        {
            result.exit_code = handle.wait();
            return result;
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        // CC-6: adaptive backoff (1->2->4...->50 ms). The fixed 50 ms poll
        // added ~25 ms mean latency to EVERY spawn — 40-80% overhead on a
        // typical 30-60 ms thumbnail render, on a counted pool slot.
        auto poll_interval = std::chrono::milliseconds(1);

        while (true)
        {
            int rc = handle.tryWait();
            if (rc >= 0)
            {
                result.exit_code = rc;
                return result;
            }

            if (std::chrono::steady_clock::now() >= deadline)
            {
                logger.error("Subprocess exceeded %d ms timeout, killing: %s", timeout_ms, command);
                try
                {
                    Poco::Process::kill(handle);
                }
                catch (const std::exception &e)
                {
                    logger.warning("Failed to kill timed-out subprocess: %s", std::string(e.what()));
                }
                // Reap the killed child so it does not linger as a zombie.
                try
                {
                    result.exit_code = handle.wait();
                }
                catch (...)
                {
                }
                result.timed_out = true;
                return result;
            }

            std::this_thread::sleep_for(poll_interval);
            poll_interval = std::min(poll_interval * 2, std::chrono::milliseconds(50));
        }
    }

} // namespace MediaDedup
