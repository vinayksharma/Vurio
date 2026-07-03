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

    DayGroupsPage DayGroupsOps::getDayGroups(DatabaseManager &db, int offset, int limit)
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

                Statement files_stmt = (sess << std::string(SQL::kSelectScanFilesByDay), use(day_key));
                files_stmt.execute();

                Poco::Data::RecordSet files_rs(files_stmt);
                for (auto &file_row : files_rs)
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

                    day.files.push_back(file);
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
}
