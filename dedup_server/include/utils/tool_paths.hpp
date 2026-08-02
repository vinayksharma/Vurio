#pragma once

#include <map>
#include <string>

namespace MediaDedup
{
    /**
     * @brief Process-wide external tool locations.
     *
     * The ImageMagick binary path defaults to /opt/homebrew/bin/magick and is
     * overridden once at server startup from the `tools.imagemagick.path`
     * config property, so static utilities (ThumbnailGenerator, transcoder)
     * don't each need a config manager plumbed through.
     */
    class ToolPaths
    {
    public:
        static void setMagickPath(const std::string &path);
        static std::string magickPath();

        /**
         * @brief Resource-limit environment for spawned `magick` processes.
         *
         * QW-4 (docs/backend_optimization_plan.md): the in-process
         * SetMagickResourceLimit calls do not cross the fork/exec boundary, so
         * without these the child runs the machine-wide defaults (13 OpenMP
         * threads, 48 GiB memory on this hardware) — one child per pool worker
         * is exactly the oversubscription the pool tries to prevent. Pass this
         * map as the env of EVERY magick launch; parallelism belongs to the
         * thread pool (one single-threaded child per worker), not to OpenMP.
         */
        static const std::map<std::string, std::string> &magickResourceEnv();
    };

} // namespace MediaDedup
