#include "database/ejected_files_ops.hpp"
#include "database/database_manager.hpp"
#include "database/sql_constants.hpp"
#include <Poco/Data/Session.h>
#include <Poco/Data/Statement.h>
#include <Poco/Data/RecordSet.h>
#include <Poco/Logger.h>
#include <algorithm>

namespace MediaDedup
{
    using namespace Poco::Data;

    bool EjectedFilesOps::ensureTable(DatabaseManager &db)
    {
        if (!db.ensureTableExists("ejected_files_history", SQL::kCreateEjectedFilesHistoryTable))
        {
            return false;
        }

        try
        {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            Poco::Data::Statement stmt(sess);

            stmt << std::string(SQL::kCreateEjectedFilesHistoryIndexEjectedPath), Poco::Data::Keywords::now;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("EjectedFilesOps").warning("Failed to create index (may already exist): " + std::string(e.what()));
        }

        return true;
    }

    bool EjectedFilesOps::insert(DatabaseManager &db, const EjectedFileHistory &history)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            return insert(lease.get(), history);
        }
        catch (...)
        {
            return false;
        }
    }

    bool EjectedFilesOps::insert(Session &sess, const EjectedFileHistory &history)
    {
        try
        {
            Statement stmt(sess);

            int file_id_val = history.file_id;
            std::string ejected_path_val = history.ejected_path;
            std::string original_file_path_val = history.original_file_path;
            std::string original_relative_path_val = history.original_relative_path;
            std::string original_location_key_val = history.original_location_key;

            stmt << std::string(SQL::kInsertEjectedFileHistory),
                Keywords::use(file_id_val),
                Keywords::use(ejected_path_val),
                Keywords::use(original_file_path_val),
                Keywords::use(original_relative_path_val),
                Keywords::use(original_location_key_val),
                Keywords::now;

            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("EjectedFilesOps").error("Failed to insert ejected file history: " + std::string(e.what()));
            return false;
        }
    }

    std::optional<EjectedFileHistory> EjectedFilesOps::getByEjectedPath(DatabaseManager &db, const std::string &ejected_path)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);

            std::string path_val = ejected_path;

            stmt << std::string(SQL::kSelectEjectedFileHistoryByPath),
                Keywords::use(path_val),
                Keywords::now;

            RecordSet rs(stmt);
            if (!rs.moveFirst())
            {
                return std::nullopt;
            }

            EjectedFileHistory history;
            history.file_id = rs[0].convert<int>();
            history.ejected_path = rs[1].convert<std::string>();
            history.original_file_path = rs[2].convert<std::string>();
            history.original_relative_path = rs[3].convert<std::string>();
            history.original_location_key = rs[4].convert<std::string>();
            history.ejected_at = rs[5].convert<std::string>();

            return history;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("EjectedFilesOps").error("Failed to get ejected file history: " + std::string(e.what()));
            return std::nullopt;
        }
    }
    std::optional<EjectedFileHistory> EjectedFilesOps::getByOriginalPath(DatabaseManager &db, const std::string &original_path)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);

            std::string path_val = original_path;

            stmt << std::string(SQL::kSelectEjectedFileHistoryByOriginalPath),
                Keywords::use(path_val),
                Keywords::now;

            RecordSet rs(stmt);
            if (!rs.moveFirst())
            {
                return std::nullopt;
            }

            EjectedFileHistory history;
            history.file_id = rs[0].convert<int>();
            history.ejected_path = rs[1].convert<std::string>();
            history.original_file_path = rs[2].convert<std::string>();
            history.original_relative_path = rs[3].convert<std::string>();
            history.original_location_key = rs[4].convert<std::string>();
            history.ejected_at = rs[5].convert<std::string>();

            return history;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("EjectedFilesOps").error("Failed to get ejected file history by original path: " + std::string(e.what()));
            return std::nullopt;
        }
    }
    std::vector<EjectedFileHistory> EjectedFilesOps::getByLocationKey(DatabaseManager &db, const std::string &location_key)
    {
        std::vector<EjectedFileHistory> results;
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);

            std::string key_val = location_key;

            stmt << std::string(SQL::kSelectEjectedFileHistoryByLocationKey),
                Keywords::use(key_val),
                Keywords::now;

            RecordSet rs(stmt);
            bool more = rs.moveFirst();
            while (more)
            {
                EjectedFileHistory history;
                history.file_id = rs[0].convert<int>();
                history.ejected_path = rs[1].convert<std::string>();
                history.original_file_path = rs[2].convert<std::string>();
                history.original_relative_path = rs[3].convert<std::string>();
                history.original_location_key = rs[4].convert<std::string>();
                history.ejected_at = rs[5].convert<std::string>();

                results.push_back(history);
                more = rs.moveNext();
            }
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("EjectedFilesOps").error("Failed to get ejected file history by location key: " + std::string(e.what()));
        }
        return results;
    }

    bool EjectedFilesOps::hasAny(DatabaseManager &db)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);

            stmt << "SELECT 1 FROM ejected_files_history LIMIT 1",
                Keywords::now;

            RecordSet rs(stmt);
            return rs.moveFirst();
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("EjectedFilesOps").error("Failed to check for any ejected files: " + std::string(e.what()));
            return false;
        }
    }

    std::vector<EjectedFileHistory> EjectedFilesOps::getAll(DatabaseManager &db)
    {
        std::vector<EjectedFileHistory> results;
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);

            stmt << std::string(SQL::kSelectAllEjectedFilesHistory),
                Keywords::now;

            RecordSet rs(stmt);
            bool more = rs.moveFirst();
            while (more)
            {
                EjectedFileHistory history;
                history.file_id = rs[0].convert<int>();
                history.ejected_path = rs[1].convert<std::string>();
                history.original_file_path = rs[2].convert<std::string>();
                history.original_relative_path = rs[3].convert<std::string>();
                history.original_location_key = rs[4].convert<std::string>();
                history.ejected_at = rs[5].convert<std::string>();

                results.push_back(history);
                more = rs.moveNext();
            }
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("EjectedFilesOps").error("Failed to get all ejected files history: " + std::string(e.what()));
        }
        return results;
    }

    bool EjectedFilesOps::deleteByEjectedPath(DatabaseManager &db, const std::string &ejected_path)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            return deleteByEjectedPath(lease.get(), ejected_path);
        }
        catch (...)
        {
            return false;
        }
    }

    bool EjectedFilesOps::deleteByEjectedPath(Session &sess, const std::string &ejected_path)
    {
        try
        {
            Statement stmt(sess);

            std::string path_val = ejected_path;

            stmt << std::string(SQL::kDeleteEjectedFileHistoryByEjectedPath),
                Keywords::use(path_val),
                Keywords::now;

            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("EjectedFilesOps").error("Failed to delete ejected file history: " + std::string(e.what()));
            return false;
        }
    }

    namespace
    {
        // Normalize separators to '/' and strip a single trailing slash.
        std::string normRoot(const std::string &p)
        {
            std::string s = p;
            std::replace(s.begin(), s.end(), '\\', '/');
            while (s.length() > 1 && s.back() == '/')
                s.pop_back();
            return s;
        }

        // If `path` (normalized) starts with `oldRoot`, replace that prefix with
        // `newRoot`; otherwise return `path` unchanged.
        std::string replacePrefix(const std::string &path, const std::string &oldRoot, const std::string &newRoot)
        {
            std::string p = path;
            std::replace(p.begin(), p.end(), '\\', '/');
            if (p.rfind(oldRoot, 0) == 0)
                return newRoot + p.substr(oldRoot.length());
            return p;
        }
    }

    int EjectedFilesOps::remapLocation(DatabaseManager &db,
                                       const std::string &old_location_key,
                                       const std::string &new_location_key,
                                       const std::string &old_root,
                                       const std::string &new_root)
    {
        const std::string old_norm = normRoot(old_root);
        const std::string new_norm = normRoot(new_root);

        auto rows = getByLocationKey(db, old_location_key);
        if (rows.empty())
            return 0;

        int updated = 0;
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            for (const auto &h : rows)
            {
                std::string new_ejected = replacePrefix(h.ejected_path, old_norm, new_norm);
                std::string new_original = replacePrefix(h.original_file_path, old_norm, new_norm);

                Statement stmt(sess);
                int file_id_val = h.file_id;
                std::string new_key_val = new_location_key;
                stmt << "UPDATE ejected_files_history SET ejected_path=?, original_file_path=?, "
                        "original_location_key=? WHERE file_id=?",
                    Keywords::use(new_ejected),
                    Keywords::use(new_original),
                    Keywords::use(new_key_val),
                    Keywords::use(file_id_val),
                    Keywords::now;
                updated++;
            }
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("EjectedFilesOps").error("Failed to remap ejected file history: " + std::string(e.what()));
        }
        return updated;
    }

    bool EjectedFilesOps::deleteAll(DatabaseManager &db)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);

            stmt << std::string(SQL::kDeleteEjectedFileHistoryAll),
                Keywords::now;

            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("EjectedFilesOps").error("Failed to delete all ejected files history: " + std::string(e.what()));
            return false;
        }
    }
}
