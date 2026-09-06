#pragma once

#include <map>
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
     * The child is placed in its OWN process group, and on timeout the whole
     * group is killed (SIGKILL) and the child reaped; the result has
     * timed_out = true. Killing the group (not just the child pid) means a
     * grandchild the tool shells out to (e.g. an ImageMagick delegate) cannot
     * survive the timeout as an orphan. A timeout_ms <= 0 waits without a
     * limit.
     *
     * `extra_env` entries are ADDED to the child's inherited environment
     * (overriding same-named inherited variables). Every magick launch must
     * pass ToolPaths::magickResourceEnv() here so the child runs
     * resource-limited (QW-4).
     *
     * When `capture_stdout` is non-null the child's stdout is redirected to a
     * pipe and accumulated into that string (drained concurrently with the
     * wait, so a chatty child cannot deadlock on a full pipe buffer).
     *
     * This is the ONLY sanctioned way to run the external `magick` binary —
     * a RAW file that stalls the decoder must never pin a pool thread forever.
     */
    SubprocessResult runProcessWithTimeout(const std::string &command,
                                           const std::vector<std::string> &args,
                                           int timeout_ms,
                                           const std::map<std::string, std::string> &extra_env = {},
                                           std::string *capture_stdout = nullptr);

} // namespace MediaDedup
