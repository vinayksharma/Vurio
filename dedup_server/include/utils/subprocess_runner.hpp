#pragma once

#include <string>
#include <vector>

namespace MediaDedup
{
    struct SubprocessResult
    {
        int exit_code = -1;
        bool timed_out = false;

        bool ok() const { return !timed_out && exit_code == 0; }
    };

    /**
     * @brief Launch an external process and wait for it with a hard timeout.
     *
     * On timeout the process is killed (SIGKILL) and reaped; the result has
     * timed_out = true. A timeout_ms <= 0 waits without a limit.
     *
     * This is the ONLY sanctioned way to run the external `magick` binary —
     * a RAW file that stalls the decoder must never pin a pool thread forever.
     */
    SubprocessResult runProcessWithTimeout(const std::string &command,
                                           const std::vector<std::string> &args,
                                           int timeout_ms);

} // namespace MediaDedup
