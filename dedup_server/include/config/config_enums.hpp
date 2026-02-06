#pragma once

#include <string>
#include <algorithm>

namespace MediaDedup
{
    // Server-wide processing mode
    // Currently only supports embedding-based duplicate detection (formerly QUALITY mode)
    enum class ServerMode
    {
        EMBEDDING // CLIP embedding-based processing (default and only mode)
    };

    inline ServerMode parseServerMode(const std::string &value)
    {
        // Always return EMBEDDING regardless of input
        // This maintains API compatibility while enforcing single mode
        return ServerMode::EMBEDDING;
    }

    inline const char *toString(ServerMode mode)
    {
        return "EMBEDDING";
    }
}
