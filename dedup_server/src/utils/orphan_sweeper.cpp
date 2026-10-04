#include "utils/orphan_sweeper.hpp"
#include "utils/subprocess_runner.hpp"

#include <Poco/Logger.h>

#include <sstream>
#include <string>
#include <unistd.h>

namespace MediaDedup
{
    namespace
    {
        // A live server owns its children and times them out itself, so a
        // pattern match is only provably an orphan when NO server is running
        // anywhere. With one instance per worktree this check has teeth:
        // sweeping while a sibling worktree's server is mid-transcode would
        // kill its healthy in-flight children. Also covers the case where the
        // config-level instance check is disabled.
        bool anotherServerIsRunning(Poco::Logger &logger)
        {
            std::string out;
            SubprocessResult run = runProcessWithTimeout("pgrep", {"-f", "media_dedup_server"},
                                                         5000, {}, &out);
            if (run.timed_out)
            {
                logger.warning("pgrep timed out; skipping orphan sweep to stay safe");
                return true;
            }
            // pgrep exits 1 when nothing matched.
            if (run.exit_code != 0)
                return false;

            const pid_t self = ::getpid();
            std::istringstream lines(out);
            std::string line;
            while (std::getline(lines, line))
            {
                try
                {
                    if (!line.empty() && static_cast<pid_t>(std::stol(line)) != self)
                        return true;
                }
                catch (...)
                {
                }
            }
            return false;
        }
    } // namespace

    void sweepOrphanedMediaSubprocesses()
    {
        Poco::Logger &logger = Poco::Logger::get("OrphanSweeper");

        if (anotherServerIsRunning(logger))
        {
            logger.information("Another server instance is running; skipping orphan sweep");
            return;
        }

        // Every media child's argv carries one of these app-specific markers
        // (temp-output names, or the exif probe's -define token), so a
        // targeted pkill is safe and precise. Keep this list in sync with
        // kill_orphaned_subprocesses in dedup_server/scripts/run.sh and the
        // sweep in the repo-root killall.
        static const char *kPatterns[] = {
            "magick_out_", "heic_out_", "thumb_out_",
            "raw_embedded_", "fullres_", "exif_probe_", "adj_raw_"};

        for (const char *pattern : kPatterns)
        {
            // pkill exits 0 iff it matched (and killed) something.
            SubprocessResult run = runProcessWithTimeout("pkill", {"-9", "-f", pattern}, 5000);
            if (run.ok())
                logger.warning("Killed orphaned media subprocess(es) matching '%s' left over from a previous run",
                               std::string(pattern));
        }
    }

} // namespace MediaDedup
