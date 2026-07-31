#pragma once

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
    };

} // namespace MediaDedup
