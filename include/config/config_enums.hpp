#pragma once

#include <string>
#include <algorithm>

namespace MediaDedup
{
    // Server-wide processing mode
    enum class ServerMode
    {
        FAST,
        BALANCED,
        QUALITY
    };

    inline ServerMode parseServerMode(const std::string &value)
    {
        std::string v = value;
        std::transform(v.begin(), v.end(), v.begin(), ::toupper);
        if (v == "BALANCED")
            return ServerMode::BALANCED;
        if (v == "QUALITY")
            return ServerMode::QUALITY;
        return ServerMode::FAST;
    }

    inline const char *toString(ServerMode mode)
    {
        switch (mode)
        {
        case ServerMode::BALANCED:
            return "BALANCED";
        case ServerMode::QUALITY:
            return "QUALITY";
        case ServerMode::FAST:
        default:
            return "FAST";
        }
    }
}
