#include "utils/tool_paths.hpp"

#include <mutex>

namespace MediaDedup
{
    namespace
    {
        std::mutex g_mutex;
        std::string g_magick_path = "/opt/homebrew/bin/magick";
    }

    void ToolPaths::setMagickPath(const std::string &path)
    {
        if (path.empty())
            return;
        std::lock_guard<std::mutex> lock(g_mutex);
        g_magick_path = path;
    }

    std::string ToolPaths::magickPath()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_magick_path;
    }

    const std::map<std::string, std::string> &ToolPaths::magickResourceEnv()
    {
        // One single-threaded child per pool worker; memory bounded with
        // graceful degradation (heap -> mmap -> disk pixel cache) instead of
        // unbounded RSS. Width/height guard against decompression bombs while
        // leaving stitched panoramas (tens of KP) comfortable headroom.
        static const std::map<std::string, std::string> env = {
            {"MAGICK_THREAD_LIMIT", "1"},
            {"MAGICK_MEMORY_LIMIT", "256MiB"},
            {"MAGICK_MAP_LIMIT", "512MiB"},
            {"MAGICK_AREA_LIMIT", "1GiB"},
            {"MAGICK_DISK_LIMIT", "2GiB"},
            {"MAGICK_WIDTH_LIMIT", "65536"},
            {"MAGICK_HEIGHT_LIMIT", "65536"},
        };
        return env;
    }

} // namespace MediaDedup
