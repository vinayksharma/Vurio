#include "database/adjustments_ops.hpp"
#include "database/database_manager.hpp"
#include "database/sql_constants.hpp"
#include <Poco/Data/RecordSet.h>
#include <Poco/Data/Session.h>
#include <Poco/Data/Statement.h>
#include <Poco/Logger.h>

using Poco::Data::Session;
using Poco::Data::Statement;
using Poco::Data::Keywords::into;
using Poco::Data::Keywords::now;
using Poco::Data::Keywords::use;

namespace MediaDedup
{
    namespace
    {
        Poco::Logger &log() { return Poco::Logger::get("AdjustmentsOps"); }

    }

    bool AdjustmentsOps::ensureTable(DatabaseManager &db)
    {
        if (!db.ensureTableExists("image_adjustments", SQL::kCreateImageAdjustmentsTable))
            return false;
        try
        {
            auto lease = db.acquireSessionLease();
            lease.get() << std::string(SQL::kCreateImageAdjustmentsIndexLocation), now;
            return true;
        }
        catch (const std::exception &e)
        {
            log().error("ensureTable: %s", std::string(e.what()));
            return false;
        }
    }

    bool AdjustmentsOps::upsert(DatabaseManager &db, const ImageAdjustmentRow &row)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            std::string path = row.file_path, key = row.location_key, doc = row.document, sidecar = row.sidecar_path;
            int rev = row.render_revision;
            int64_t smtime = row.sidecar_mtime, updated = row.updated_at;
            stmt << std::string(SQL::kUpsertImageAdjustment),
                use(path), use(key), use(rev), use(doc), use(sidecar), use(smtime), use(updated), now;
            return true;
        }
        catch (const std::exception &e)
        {
            log().error("upsert %s: %s", row.file_path, std::string(e.what()));
            return false;
        }
    }

    std::optional<ImageAdjustmentRow> AdjustmentsOps::get(DatabaseManager &db, const std::string &file_path)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            std::string path = file_path;
            stmt << std::string(SQL::kSelectImageAdjustmentByPath), use(path);
            stmt.execute();
            Poco::Data::RecordSet rs(stmt);
            if (!rs.moveFirst())
                return std::nullopt;
            ImageAdjustmentRow row;
            row.file_path = rs[0].convert<std::string>();
            row.location_key = rs[1].convert<std::string>();
            row.render_revision = rs[2].convert<int>();
            row.document = rs[3].convert<std::string>();
            row.sidecar_path = rs[4].convert<std::string>();
            row.sidecar_mtime = rs[5].convert<int64_t>();
            row.updated_at = rs[6].convert<int64_t>();
            return row;
        }
        catch (const std::exception &e)
        {
            log().error("get %s: %s", file_path, std::string(e.what()));
            return std::nullopt;
        }
    }

    namespace
    {
        std::vector<ImageAdjustmentRow> readAll(Statement &stmt)
        {
            std::vector<ImageAdjustmentRow> out;
            stmt.execute();
            Poco::Data::RecordSet rs(stmt);
            bool more = rs.moveFirst();
            while (more)
            {
                ImageAdjustmentRow row;
                row.file_path = rs[0].convert<std::string>();
                row.location_key = rs[1].convert<std::string>();
                row.render_revision = rs[2].convert<int>();
                row.document = rs[3].convert<std::string>();
                row.sidecar_path = rs[4].convert<std::string>();
                row.sidecar_mtime = rs[5].convert<int64_t>();
                row.updated_at = rs[6].convert<int64_t>();
                out.push_back(std::move(row));
                more = rs.moveNext();
            }
            return out;
        }
    }

    std::vector<ImageAdjustmentRow> AdjustmentsOps::listAll(DatabaseManager &db)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Statement stmt(lease.get());
            stmt << std::string(SQL::kSelectImageAdjustmentColumns);
            return readAll(stmt);
        }
        catch (const std::exception &e)
        {
            log().error("listAll: %s", std::string(e.what()));
            return {};
        }
    }

    std::vector<ImageAdjustmentRow> AdjustmentsOps::listByLocation(DatabaseManager &db, const std::string &location_key)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Statement stmt(lease.get());
            std::string key = location_key;
            stmt << std::string(SQL::kSelectImageAdjustmentsByLocation), use(key);
            return readAll(stmt);
        }
        catch (const std::exception &e)
        {
            log().error("listByLocation %s: %s", location_key, std::string(e.what()));
            return {};
        }
    }

    bool AdjustmentsOps::remove(DatabaseManager &db, const std::string &file_path)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            std::string path = file_path;
            lease.get() << std::string(SQL::kDeleteImageAdjustmentByPath), use(path), now;
            return true;
        }
        catch (const std::exception &e)
        {
            log().error("remove %s: %s", file_path, std::string(e.what()));
            return false;
        }
    }

    bool AdjustmentsOps::removeByLocation(DatabaseManager &db, const std::string &location_key)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            std::string key = location_key;
            lease.get() << std::string(SQL::kDeleteImageAdjustmentsByLocation), use(key), now;
            return true;
        }
        catch (const std::exception &e)
        {
            log().error("removeByLocation %s: %s", location_key, std::string(e.what()));
            return false;
        }
    }

    bool AdjustmentsOps::updateSidecar(DatabaseManager &db, const std::string &file_path,
                                       const std::string &sidecar_path, int64_t sidecar_mtime)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            std::string path = file_path, sidecar = sidecar_path;
            int64_t mtime = sidecar_mtime;
            lease.get() << std::string(SQL::kUpdateImageAdjustmentSidecar), use(sidecar), use(mtime), use(path), now;
            return true;
        }
        catch (const std::exception &e)
        {
            log().error("updateSidecar %s: %s", file_path, std::string(e.what()));
            return false;
        }
    }

    bool AdjustmentsOps::updateFilePath(DatabaseManager &db, const std::string &old_path,
                                        const std::string &new_path, const std::string &new_location_key)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            std::string from = old_path, to = new_path, key = new_location_key;
            lease.get() << std::string(SQL::kUpdateImageAdjustmentFilePath), use(to), use(key), use(from), now;
            return true;
        }
        catch (const std::exception &e)
        {
            log().error("updateFilePath %s: %s", old_path, std::string(e.what()));
            return false;
        }
    }

    bool AdjustmentsOps::counts(DatabaseManager &db, const std::string &location_key, int &rows, int &spooled)
    {
        rows = 0;
        spooled = 0;
        try
        {
            auto lease = db.acquireSessionLease();
            Statement stmt(lease.get());
            std::string key = location_key;
            stmt << std::string(SQL::kSelectImageAdjustmentCountsByLocation), use(key);
            stmt.execute();
            Poco::Data::RecordSet rs(stmt);
            if (rs.moveFirst())
            {
                rows = rs[0].isEmpty() ? 0 : rs[0].convert<int>();
                spooled = rs[1].isEmpty() ? 0 : rs[1].convert<int>();
            }
            return true;
        }
        catch (const std::exception &e)
        {
            log().error("counts %s: %s", location_key, std::string(e.what()));
            return false;
        }
    }
}
