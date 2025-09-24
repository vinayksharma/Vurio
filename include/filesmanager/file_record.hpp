#pragma once
#include <cstdint>
#include <string>
#include <chrono>
#include <filesystem>
#include "filesmanager/file_scanner_errors.hpp"

namespace MediaDedup::Files
{
    using TimePoint = std::filesystem::file_time_type;

    struct FileRecord
    {
        std::string fileName;
        std::string fullPath;
        std::string extension;

        std::string networkSharePath;
        std::string networkShareDriveName;
        bool isShareMapped = false;

        TimePoint createdAt{};
        TimePoint modifiedAt{};
        std::uint64_t fileSizeBytes = 0;

        std::string deviceId;
        std::string inode;

        std::string symlinkTarget;
        bool isHidden = false;

        ErrorCode error = ErrorCode::OK;
        int platformErrno = 0;
        std::string errorMessage;

        bool hasError() const noexcept { return error != ErrorCode::OK; }
    };

    enum class ChangeKind
    {
        Unchanged,
        SizeOrTimeChanged,
        IdentityChanged,
        AttributesChanged
    };
}
