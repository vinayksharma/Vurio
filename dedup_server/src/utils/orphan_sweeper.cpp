#include "utils/orphan_sweeper.hpp"
#include "utils/subprocess_runner.hpp"

#include <Poco/Logger.h>

namespace MediaDedup
{
    void sweepOrphanedMediaSubprocesses()
    {
        // Every media child's argv carries one of these app-specific markers
        // (temp-output names, or the exif probe's -define token), so a
        // targeted pkill is safe and precise. Keep this list in sync with
        // kill_orphaned_subprocesses in scripts/run.sh and scripts/stopall.
        static const char *kPatterns[] = {
            "magick_out_", "heic_out_", "thumb_out_",
            "raw_embedded_", "fullres_", "exif_probe_"};

        Poco::Logger &logger = Poco::Logger::get("OrphanSweeper");
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
