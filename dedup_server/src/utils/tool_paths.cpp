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

} // namespace MediaDedup
