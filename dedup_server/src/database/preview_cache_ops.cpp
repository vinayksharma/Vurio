#include "database/preview_cache_ops.hpp"
#include "database/database_manager.hpp"
#include "database/sql_constants.hpp"
#include <Poco/Logger.h>
#include <Poco/Data/Session.h>
#include <Poco/Data/Statement.h>
#include <chrono>
#include <filesystem>

using Poco::Data::Session;
using Poco::Data::Statement;
using Poco::Data::Keywords::into;
using Poco::Data::Keywords::now;
using Poco::Data::Keywords::use;

namespace MediaDedup
{
    bool PreviewCacheOps::ensureTable(DatabaseManager &db)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();

            // Create table
            sess << std::string(SQL::kCreatePreviewCacheTable), now;

            // Create indices
            sess << std::string(SQL::kCreatePreviewCacheIndexSourcePath), now;
            sess << std::string(SQL::kCreatePreviewCacheIndexSourceModified), now;
            sess << std::string(SQL::kCreatePreviewCacheIndexLastAccessed), now;

            Poco::Logger::get("PreviewCacheOps").debug("Preview cache table and indices ensured");
            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("PreviewCacheOps").error("Failed to ensure preview_cache table: %s", std::string(e.what()));
            return false;
        }
    }

    std::optional<PreviewCacheRecord> PreviewCacheOps::getPreview(DatabaseManager &db, const std::string &source_path,
                                                                  bool *lookup_failed)
    {
        if (lookup_failed)
            *lookup_failed = false;
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);

            PreviewCacheRecord record;
            std::string src = source_path;

            stmt << std::string(SQL::kSelectPreviewByPath),
                into(record.id),
                into(record.source_path),
                into(record.cached_path),
                into(record.file_size_bytes),
                into(record.source_modified_at),
                into(record.created_at),
                into(record.last_accessed_at),
                use(src);

            if (stmt.execute() > 0)
            {
                Poco::Logger::get("PreviewCacheOps").debug("Found cached preview for: %s", source_path);
                return record;
            }

            return std::nullopt;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("PreviewCacheOps").error("Exception in getPreview: %s", std::string(e.what()));
            if (lookup_failed)
                *lookup_failed = true;
            return std::nullopt;
        }
    }

    bool PreviewCacheOps::upsertPreview(DatabaseManager &db, const PreviewCacheRecord &record)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);

            std::string src = record.source_path;
            std::string cached = record.cached_path;
            int64_t fsize = record.file_size_bytes;
            int64_t mod = record.source_modified_at;
            int64_t created = record.created_at;
            int64_t accessed = record.last_accessed_at;

            stmt << std::string(SQL::kUpsertPreviewCache),
                use(src),
                use(cached),
                use(fsize),
                use(mod),
                use(created),
                use(accessed),
                now;

            Poco::Logger::get("PreviewCacheOps").debug("Upserted preview cache entry: %s", record.source_path);
            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("PreviewCacheOps").error("Exception in upsertPreview: %s", std::string(e.what()));
            return false;
        }
    }

    bool PreviewCacheOps::updateAccessTime(DatabaseManager &db, const std::string &source_path, int64_t timestamp)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);

            int64_t ts = timestamp;
            std::string src = source_path;

            stmt << std::string(SQL::kUpdatePreviewAccessTime),
                use(ts),
                use(src),
                now;

            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("PreviewCacheOps").error("Exception in updateAccessTime: %s", std::string(e.what()));
            return false;
        }
    }

    bool PreviewCacheOps::deletePreview(DatabaseManager &db, const std::string &source_path)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);

            std::string src = source_path;

            stmt << std::string(SQL::kDeletePreviewByPath), use(src), now;
            Poco::Logger::get("PreviewCacheOps").debug("Deleted preview entry for: %s", source_path);

            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("PreviewCacheOps").error("Exception in deletePreview: %s", std::string(e.what()));
            return false;
        }
    }

    int PreviewCacheOps::deleteStaleEntries(DatabaseManager &db, int64_t before_timestamp)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);

            int64_t ts = before_timestamp;

            stmt << std::string(SQL::kSelectStalePreviews), use(ts);

            int count = 0;
            stmt.execute();

            Statement delete_stmt(sess);
            delete_stmt << std::string(SQL::kSelectStalePreviews), use(ts);

            Poco::Logger::get("PreviewCacheOps").information("Deleted %d stale preview entries", count);
            return count;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("PreviewCacheOps").error("Exception in deleteStaleEntries: %s", std::string(e.what()));
            return 0;
        }
    }

    int PreviewCacheOps::getCount(DatabaseManager &db)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();

            int count = 0;
            sess << std::string(SQL::kCountPreviewCache), into(count), now;

            return count;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("PreviewCacheOps").error("Exception in getCount: %s", std::string(e.what()));
            return 0;
        }
    }

    bool PreviewCacheOps::clearCache(DatabaseManager &db)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();

            sess << "DELETE FROM preview_cache", now;
            
            Poco::Logger::get("PreviewCacheOps").information("Cleared all entries from preview cache");
            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("PreviewCacheOps").error("Exception in clearCache: %s", std::string(e.what()));
            return false;
        }
    }

    bool PreviewCacheOps::updateCacheLocation(DatabaseManager &db, const std::string &old_base_path, const std::string &new_base_path)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);

            std::string old_path = old_base_path;
            std::string new_path = new_base_path;
            
            // Ensure paths end with a separator so we don't accidentally match partial paths
            if (!old_path.empty() && old_path.back() != '/' && old_path.back() != '\\') old_path += '/';
            if (!new_path.empty() && new_path.back() != '/' && new_path.back() != '\\') new_path += '/';

            stmt << std::string(SQL::kUpdatePreviewCacheLocation), 
                Poco::Data::Keywords::use(old_path), 
                Poco::Data::Keywords::use(new_path), 
                Poco::Data::Keywords::use(old_path),
                Poco::Data::Keywords::now;

            Poco::Logger::get("PreviewCacheOps").information("Updated preview cache locations from %s to %s", old_base_path, new_base_path);
            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("PreviewCacheOps").error("Exception in updateCacheLocation: %s", std::string(e.what()));
            return false;
        }
    }

    PreviewCacheRecord PreviewCacheOps::makeRecord(const std::string &source_path,
                                                   const std::string &cached_path)
    {
        PreviewCacheRecord record;
        record.source_path = source_path;
        record.cached_path = cached_path;

        try
        {
            auto ftime = std::filesystem::last_write_time(source_path);
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
            record.source_modified_at = std::chrono::system_clock::to_time_t(sctp);
        }
        catch (...)
        {
            record.source_modified_at = 0;
        }

        try
        {
            record.file_size_bytes = static_cast<int64_t>(std::filesystem::file_size(cached_path));
        }
        catch (...)
        {
            record.file_size_bytes = 0;
        }

        int64_t now_ts = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        record.created_at = now_ts;
        record.last_accessed_at = now_ts;
        return record;
    }

} // namespace MediaDedup
