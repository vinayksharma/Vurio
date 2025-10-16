#include "database/thumbnail_cache_ops.hpp"
#include "database/database_manager.hpp"
#include "database/sql_constants.hpp"
#include <Poco/Logger.h>
#include <Poco/Data/Session.h>
#include <Poco/Data/Statement.h>

using Poco::Data::Session;
using Poco::Data::Statement;
using Poco::Data::Keywords::into;
using Poco::Data::Keywords::now;
using Poco::Data::Keywords::use;

namespace MediaDedup
{
    bool ThumbnailCacheOps::ensureTable(DatabaseManager &db)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();

            // Create table
            sess << std::string(SQL::kCreateThumbnailCacheTable), now;

            // Create indices
            sess << std::string(SQL::kCreateThumbnailCacheIndexSourcePath), now;
            sess << std::string(SQL::kCreateThumbnailCacheIndexSourceModified), now;
            sess << std::string(SQL::kCreateThumbnailCacheIndexLastAccessed), now;

            Poco::Logger::get("ThumbnailCacheOps").debug("Thumbnail cache table and indices ensured");
            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ThumbnailCacheOps").error("Failed to ensure thumbnail_cache table: %s", std::string(e.what()));
            return false;
        }
    }

    std::optional<ThumbnailCacheRecord> ThumbnailCacheOps::getThumbnail(DatabaseManager &db,
                                                                        const std::string &source_path,
                                                                        int size)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);

            ThumbnailCacheRecord record;
            std::string src = source_path;
            int sz = size;

            stmt << std::string(SQL::kSelectThumbnailByPath),
                into(record.id),
                into(record.source_path),
                into(record.cached_path),
                into(record.thumbnail_size),
                into(record.file_size_bytes),
                into(record.source_modified_at),
                into(record.created_at),
                into(record.last_accessed_at),
                use(src),
                use(sz);

            if (stmt.execute() > 0)
            {
                Poco::Logger::get("ThumbnailCacheOps").debug("Found cached thumbnail for: %s (size: %d)", source_path, size);
                return record;
            }

            return std::nullopt;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ThumbnailCacheOps").error("Exception in getThumbnail: %s", std::string(e.what()));
            return std::nullopt;
        }
    }

    bool ThumbnailCacheOps::upsertThumbnail(DatabaseManager &db, const ThumbnailCacheRecord &record)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);

            std::string src = record.source_path;
            std::string cached = record.cached_path;
            int sz = record.thumbnail_size;
            int64_t fsize = record.file_size_bytes;
            int64_t mod = record.source_modified_at;
            int64_t created = record.created_at;
            int64_t accessed = record.last_accessed_at;

            stmt << std::string(SQL::kUpsertThumbnailCache),
                use(src),
                use(cached),
                use(sz),
                use(fsize),
                use(mod),
                use(created),
                use(accessed),
                now;

            Poco::Logger::get("ThumbnailCacheOps").debug("Upserted thumbnail cache entry: %s (size: %d)", record.source_path, sz);
            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ThumbnailCacheOps").error("Exception in upsertThumbnail: %s", std::string(e.what()));
            return false;
        }
    }

    bool ThumbnailCacheOps::updateAccessTime(DatabaseManager &db,
                                             const std::string &source_path,
                                             int size,
                                             int64_t timestamp)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);

            int64_t ts = timestamp;
            std::string src = source_path;
            int sz = size;

            stmt << std::string(SQL::kUpdateThumbnailAccessTime),
                use(ts),
                use(src),
                use(sz),
                now;

            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ThumbnailCacheOps").error("Exception in updateAccessTime: %s", std::string(e.what()));
            return false;
        }
    }

    bool ThumbnailCacheOps::deleteThumbnail(DatabaseManager &db,
                                            const std::string &source_path,
                                            int size)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);

            std::string src = source_path;

            if (size == 0)
            {
                // Delete all sizes for this path
                stmt << std::string(SQL::kDeleteThumbnailByPath), use(src), now;
                Poco::Logger::get("ThumbnailCacheOps").debug("Deleted all thumbnail entries for: %s", source_path);
            }
            else
            {
                // Delete specific size
                int sz = size;
                stmt << std::string(SQL::kDeleteThumbnailByPathAndSize), use(src), use(sz), now;
                Poco::Logger::get("ThumbnailCacheOps").debug("Deleted thumbnail entry for: %s (size: %d)", source_path, size);
            }

            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ThumbnailCacheOps").error("Exception in deleteThumbnail: %s", std::string(e.what()));
            return false;
        }
    }

    int ThumbnailCacheOps::deleteStaleEntries(DatabaseManager &db, int64_t before_timestamp)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);

            int64_t ts = before_timestamp;

            stmt << std::string(SQL::kSelectStaleThumbnails), use(ts);

            // Count how many will be deleted
            int count = 0;
            stmt.execute();

            // Now delete them
            Statement delete_stmt(sess);
            delete_stmt << std::string(SQL::kSelectStaleThumbnails), use(ts);

            Poco::Logger::get("ThumbnailCacheOps").information("Deleted %d stale thumbnail entries", count);
            return count;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ThumbnailCacheOps").error("Exception in deleteStaleEntries: %s", std::string(e.what()));
            return 0;
        }
    }

    int ThumbnailCacheOps::getCount(DatabaseManager &db)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();

            int count = 0;
            sess << std::string(SQL::kCountThumbnailCache), into(count), now;

            return count;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ThumbnailCacheOps").error("Exception in getCount: %s", std::string(e.what()));
            return 0;
        }
    }

} // namespace MediaDedup
