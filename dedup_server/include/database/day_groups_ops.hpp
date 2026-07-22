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
        std::vector<DayFileRecord> files; // ascending taken_time; may be a
                                          // prefix when files_per_day is set
        int total_files = 0; // total files in this day
    };

    struct DayFilesPage
    {
        std::vector<DayFileRecord> files; // ascending taken_time
        int total_files = 0; // total files in this day
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
        // files_per_day caps how many files each returned day carries
        // (0 = all files); DayGroupRecord.total_files always holds the full
        // per-day count so callers can page the rest via getDayFiles.
        static DayGroupsPage getDayGroups(DatabaseManager &db, int offset, int limit,
                                          int files_per_day = 0);

        // One page of a single day's files (taken_time order), for
        // horizontal-scroll pagination within a day row.
        static DayFilesPage getDayFiles(DatabaseManager &db, const std::string &day,
                                        int offset, int limit);
    };
}
