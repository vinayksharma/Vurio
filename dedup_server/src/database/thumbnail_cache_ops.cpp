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

    std::optional<ThumbnailCacheRecord> ThumbnailCacheOps::getBestFitThumbnail(DatabaseManager &db,
                                                                        const std::string &source_path,
                                                                        int min_size,
                                                                        bool *lookup_failed)
    {
        if (lookup_failed)
            *lookup_failed = false;
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);

            ThumbnailCacheRecord record;
            std::string src = source_path;
            int sz = min_size;

            stmt << std::string(SQL::kSelectBestFitThumbnail),
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
                Poco::Logger::get("ThumbnailCacheOps").debug("Found best fit thumbnail for: %s (requested min_size: %d, actual: %d)", source_path, min_size, record.thumbnail_size);
                return record;
            }

            return std::nullopt;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ThumbnailCacheOps").error("Exception in getBestFitThumbnail: %s", std::string(e.what()));
            if (lookup_failed)
                *lookup_failed = true;
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

    bool ThumbnailCacheOps::clearCache(DatabaseManager &db)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();

            sess << "DELETE FROM thumbnail_cache", now;
            
            Poco::Logger::get("ThumbnailCacheOps").information("Cleared all entries from thumbnail cache");
            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ThumbnailCacheOps").error("Exception in clearCache: %s", std::string(e.what()));
            return false;
        }
    }

    bool ThumbnailCacheOps::updateCacheLocation(DatabaseManager &db, const std::string &old_base_path, const std::string &new_base_path)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);

            std::string old_path = old_base_path;
            std::string new_path = new_base_path;
            
            // Ensure paths end with a separator so we don't accidentally match partial paths
            // e.g. /data/cache vs /data/cache_old
            if (!old_path.empty() && old_path.back() != '/' && old_path.back() != '\\') old_path += '/';
            if (!new_path.empty() && new_path.back() != '/' && new_path.back() != '\\') new_path += '/';

            stmt << std::string(SQL::kUpdateThumbnailCacheLocation), 
                Poco::Data::Keywords::use(old_path), 
                Poco::Data::Keywords::use(new_path), 
                Poco::Data::Keywords::use(old_path),
                Poco::Data::Keywords::now;

            Poco::Logger::get("ThumbnailCacheOps").information("Updated thumbnail cache locations from %s to %s", old_base_path, new_base_path);
            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ThumbnailCacheOps").error("Exception in updateCacheLocation: %s", std::string(e.what()));
            return false;
        }
    }


    std::vector<std::string> ThumbnailCacheOps::lruCachedPaths(DatabaseManager &db, int limit)
    {
        std::vector<std::string> paths;
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            stmt << "SELECT cached_path FROM thumbnail_cache ORDER BY last_accessed_at ASC LIMIT ?",
                into(paths), use(limit), now;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ThumbnailCacheOps").error("lruCachedPaths failed: %s", std::string(e.what()));
        }
        return paths;
    }

    bool ThumbnailCacheOps::deleteByCachedPaths(DatabaseManager &db, const std::vector<std::string> &cached_paths)
    {
        if (cached_paths.empty())
            return true;
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            constexpr std::size_t kChunk = 500;
            for (std::size_t start = 0; start < cached_paths.size(); start += kChunk)
            {
                std::vector<std::string> chunk(cached_paths.begin() + start,
                                               cached_paths.begin() + std::min(cached_paths.size(), start + kChunk));
                std::string placeholders;
                for (std::size_t i = 0; i < chunk.size(); ++i)
                    placeholders += i ? ",?" : "?";
                Statement stmt(sess);
                stmt << "DELETE FROM thumbnail_cache WHERE cached_path IN (" + placeholders + ")";
                for (auto &p : chunk)
                    stmt, use(p);
                stmt.execute();
            }
            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ThumbnailCacheOps").error("deleteByCachedPaths failed: %s", std::string(e.what()));
            return false;
        }
    }

} // namespace MediaDedup
