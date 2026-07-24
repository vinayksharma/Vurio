#include "database/day_groups_ops.hpp"
#include "database/database_manager.hpp"
#include "database/sql_constants.hpp"
#include <Poco/Data/Session.h>
#include <Poco/Data/Statement.h>
#include <Poco/Data/RecordSet.h>
#include <Poco/Logger.h>
#include <string>

namespace MediaDedup
{
    using namespace Poco::Data;
    using Poco::Data::Keywords::into;
    using Poco::Data::Keywords::use;

    namespace
    {
        DayFileRecord parseDayFileRow(Poco::Data::Row &file_row)
        {
            DayFileRecord file;
            file.file_id = file_row[0].convert<int>();
            file.file_path = file_row[1].convert<std::string>();
            if (!file_row[2].isEmpty())
            {
                file.taken_time = file_row[2].convert<std::string>();
            }
            file.size_bytes = file_row[3].convert<int64_t>();
            if (!file_row[4].isEmpty())
            {
                file.cached_thumbnail_path = file_row[4].convert<std::string>();
            }
            if (!file_row[5].isEmpty())
            {
                file.group_id = file_row[5].convert<int>();
                file.is_representative = file_row[6].convert<bool>();
                file.similarity_score = file_row[7].convert<double>();
            }
            return file;
        }

        int countFilesForDay(Session &sess, const std::string &day_key)
        {
            int total = 0;
            std::string key = day_key;
            std::string key2 = day_key; // bound twice: anchor branch + pulled-members branch
            Statement count_stmt = (sess << std::string(SQL::kSelectScanFilesByDayCount),
                                    use(key), use(key2), into(total));
            count_stmt.execute();
            return total;
        }
    }

    DayGroupsPage DayGroupsOps::getDayGroups(DatabaseManager &db, int offset, int limit,
                                             int files_per_day)
    {
        Poco::Logger &logger = Poco::Logger::get("DayGroupsOps");
        DayGroupsPage page;
        page.start = offset;
        page.end = offset + limit;

        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();

            Statement count_stmt = (sess << std::string(SQL::kSelectScanDaysCount), into(page.total_days));
            count_stmt.execute();

            Statement days_stmt = (sess << std::string(SQL::kSelectScanDaysWithPagination),
                                   use(limit),
                                   use(offset));
            days_stmt.execute();

            // Collect the day keys first; the per-day query below reuses the
            // same session and Poco statements cannot be interleaved.
            std::vector<std::string> day_keys;
            Poco::Data::RecordSet days_rs(days_stmt);
            for (auto &row : days_rs)
            {
                if (!row[0].isEmpty())
                {
                    day_keys.push_back(row[0].convert<std::string>());
                }
            }

            for (auto &day_key : day_keys)
            {
                DayGroupRecord day;
                day.day = day_key;

                std::string day_key2 = day_key; // bound twice: anchor + pulled-members branches
                if (files_per_day > 0)
                {
                    day.total_files = countFilesForDay(sess, day_key);

                    int file_offset = 0;
                    Statement files_stmt = (sess << std::string(SQL::kSelectScanFilesByDayPaged),
                                            use(day_key), use(day_key2), use(files_per_day), use(file_offset));
                    files_stmt.execute();

                    Poco::Data::RecordSet files_rs(files_stmt);
                    for (auto &file_row : files_rs)
                    {
                        day.files.push_back(parseDayFileRow(file_row));
                    }
                }
                else
                {
                    Statement files_stmt = (sess << std::string(SQL::kSelectScanFilesByDay),
                                            use(day_key), use(day_key2));
                    files_stmt.execute();

                    Poco::Data::RecordSet files_rs(files_stmt);
                    for (auto &file_row : files_rs)
                    {
                        day.files.push_back(parseDayFileRow(file_row));
                    }
                    day.total_files = static_cast<int>(day.files.size());
                }

                page.days.push_back(std::move(day));
            }

            page.returned = static_cast<int>(page.days.size());
            logger.debug("Retrieved %d day groups (offset=%d, limit=%d, total=%d)",
                         page.returned, offset, limit, page.total_days);

            return page;
        }
        catch (const Poco::Exception &e)
        {
            logger.error("Exception in getDayGroups: %s", e.displayText());
            return page; // Return empty page on error
        }
        catch (const std::exception &e)
        {
            logger.error("Exception in getDayGroups: %s", std::string(e.what()));
            return page; // Return empty page on error
        }
    }

    DayFilesPage DayGroupsOps::getDayFiles(DatabaseManager &db, const std::string &day,
                                           int offset, int limit)
    {
        Poco::Logger &logger = Poco::Logger::get("DayGroupsOps");
        DayFilesPage page;

        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();

            std::string day_key = day;
            std::string day_key2 = day; // bound twice: anchor + pulled-members branches
            page.total_files = countFilesForDay(sess, day_key);

            Statement files_stmt = (sess << std::string(SQL::kSelectScanFilesByDayPaged),
                                    use(day_key), use(day_key2), use(limit), use(offset));
            files_stmt.execute();

            Poco::Data::RecordSet files_rs(files_stmt);
            for (auto &file_row : files_rs)
            {
                page.files.push_back(parseDayFileRow(file_row));
            }

            logger.debug("Retrieved %d day files (day=%s, offset=%d, limit=%d, total=%d)",
                         static_cast<int>(page.files.size()), day, offset, limit, page.total_files);

            return page;
        }
        catch (const Poco::Exception &e)
        {
            logger.error("Exception in getDayFiles: %s", e.displayText());
            return page;
        }
        catch (const std::exception &e)
        {
            logger.error("Exception in getDayFiles: %s", std::string(e.what()));
            return page;
        }
    }
}
