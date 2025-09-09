#pragma once
#include <string>

namespace MediaDedup::Files
{
    enum class ErrorCode
    {
        OK = 0,
        DIR_NOT_FOUND,
        PERMISSION_DENIED,
        SHARE_OFFLINE,
        STAT_FAILED,
        SYMLINK_LOOP,
        PATH_UNREADABLE,
        UNKNOWN
    };

    inline const char *to_string(ErrorCode c)
    {
        switch (c)
        {
        case ErrorCode::OK:
            return "OK";
        case ErrorCode::DIR_NOT_FOUND:
            return "DIR_NOT_FOUND";
        case ErrorCode::PERMISSION_DENIED:
            return "PERMISSION_DENIED";
        case ErrorCode::SHARE_OFFLINE:
            return "SHARE_OFFLINE";
        case ErrorCode::STAT_FAILED:
            return "STAT_FAILED";
        case ErrorCode::SYMLINK_LOOP:
            return "SYMLINK_LOOP";
        case ErrorCode::PATH_UNREADABLE:
            return "PATH_UNREADABLE";
        default:
            return "UNKNOWN";
        }
    }
}
