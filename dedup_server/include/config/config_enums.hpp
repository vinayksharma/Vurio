#pragma once

#include <string>
#include <algorithm>
#include <vector>

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

    enum class PurityCriterion
    {
        CAMERA_RAW,
        EXIF_CAPTURE_DATE,
        FILE_SIZE
    };

    inline const char *toString(PurityCriterion criterion)
    {
        switch (criterion)
        {
        case PurityCriterion::CAMERA_RAW:
            return "CAMERA_RAW";
        case PurityCriterion::EXIF_CAPTURE_DATE:
            return "EXIF_CAPTURE_DATE";
        case PurityCriterion::FILE_SIZE:
            return "FILE_SIZE";
        default:
            return "UNKNOWN";
        }
    }

    inline std::vector<PurityCriterion> parsePurityOrder(const std::string &value)
    {
        std::vector<PurityCriterion> order;
        if (value.empty())
        {
            return {PurityCriterion::CAMERA_RAW, PurityCriterion::EXIF_CAPTURE_DATE, PurityCriterion::FILE_SIZE};
        }

        size_t start = 0;
        size_t end = value.find(',');
        while (start < value.length())
        {
            std::string token = value.substr(start, end - start);
            
            // Trim whitespace
            token.erase(0, token.find_first_not_of(" \t\r\n"));
            token.erase(token.find_last_not_of(" \t\r\n") + 1);

            // Convert to upper
            std::transform(token.begin(), token.end(), token.begin(), ::toupper);

            if (token == "CAMERA_RAW")
            {
                order.push_back(PurityCriterion::CAMERA_RAW);
            }
            else if (token == "EXIF_CAPTURE_DATE")
            {
                order.push_back(PurityCriterion::EXIF_CAPTURE_DATE);
            }
            else if (token == "FILE_SIZE")
            {
                order.push_back(PurityCriterion::FILE_SIZE);
            }

            if (end == std::string::npos)
            {
                break;
            }
            start = end + 1;
            end = value.find(',', start);
        }

        // Fallback default if completely empty due to unparseable items
        if (order.empty())
        {
            return {PurityCriterion::CAMERA_RAW, PurityCriterion::EXIF_CAPTURE_DATE, PurityCriterion::FILE_SIZE};
        }

        return order;
    }
}
