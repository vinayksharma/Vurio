#pragma once

#include <string>
#include <vector>

namespace MediaDedup
{
    class DatabaseManager;

    struct DayFileRecord
    {
        int file_id = 0;
        std::string file_path;
        std::string taken_time; // normalized "YYYY-MM-DD HH:MM:SS"
        int64_t size_bytes = 0;
        std::string cached_thumbnail_path;
        int group_id = 0; // 0 = not part of any duplicate group
        bool is_representative = false;
        double similarity_score = 0.0;
    };

    struct DayGroupRecord
    {
        std::string day; // "YYYY-MM-DD"
        std::vector<DayFileRecord> files; // ascending taken_time
    };

    struct DayGroupsPage
    {
        std::vector<DayGroupRecord> days;
        int total_days = 0;
        int start = 0;
        int end = 0;
        int returned = 0;
    };

    // Read-only queries backing the day-based "ALL" view: every scanned file
    // (not ignored, not errored) grouped by capture day. Capture day comes
    // from exif_datetime, falling back to file mtime (file_metadata JSON),
    // then the row's created_at — see SQL::kSelectScanFilesByDay.
    class DayGroupsOps
    {
    public:
        static DayGroupsPage getDayGroups(DatabaseManager &db, int offset, int limit);
    };
}
