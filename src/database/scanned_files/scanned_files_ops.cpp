#include "database/scanned_files_ops.hpp"
#include "config/config_enums.hpp"
#include "database/database_manager.hpp"
#include "database/sql_constants.hpp"
#include <Poco/Data/Session.h>
#include <Poco/Data/Statement.h>
#include <Poco/Data/RecordSet.h>

namespace MediaDedup
{
    using namespace Poco::Data;

    bool ScannedFilesOps::ensureTable(DatabaseManager &db)
    {
        // Create table first
        if (!db.ensureTableExists("scanned_files", SQL::kCreateScannedFilesTable))
        {
            return false;
        }

        // Create index for efficient file_path lookups (critical for FilesManager performance)
        // Note: UNIQUE constraint on file_path already creates an implicit index, but we add explicit one for clarity
        try
        {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            Poco::Data::Statement stmt(sess);
            stmt << std::string(SQL::kCreateScannedFilesIndexFilePath), Poco::Data::Keywords::now;
            Poco::Logger::get("ScannedFilesOps").debug("Created index on scanned_files.file_path");
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ScannedFilesOps").warning("Failed to create index on file_path (may already exist): " + std::string(e.what()));
            // Don't fail - table exists, index is just an optimization (UNIQUE already creates implicit index)
        }

        return true;
    }

    static void bindUpsertParams(Statement &stmt, const ScannedFileRow &r)
    {
        std::string file_path = r.file_path;
        std::string relative_path = r.relative_path;
        std::string share_name = r.share_name;
        std::string file_name = r.file_name;
        std::string file_metadata = r.file_metadata;
        int pf = r.processed_fast; // Preserve state code (0,1,2,...)
        int pb = r.processed_balanced;
        int pq = r.processed_quality;
        std::string lf = r.links_fast;
        std::string lb = r.links_balanced;
        std::string lq = r.links_quality;
        int inf = r.is_network_file ? 1 : 0;
        stmt << std::string(SQL::kUpsertScannedFile),
            Keywords::use(file_path),
            Keywords::use(relative_path),
            Keywords::use(share_name),
            Keywords::use(file_name),
            Keywords::use(file_metadata),
            Keywords::use(pf),
            Keywords::use(pb),
            Keywords::use(pq),
            Keywords::use(lf),
            Keywords::use(lb),
            Keywords::use(lq),
            Keywords::use(inf),
            Keywords::now;
    }

    bool ScannedFilesOps::upsert(DatabaseManager &db, const ScannedFileRow &row)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            bindUpsertParams(stmt, row);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool ScannedFilesOps::removeByPath(DatabaseManager &db, const std::string &file_path)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            std::string p = file_path;
            stmt << std::string(SQL::kDeleteScannedFile), Keywords::use(p), Keywords::now;
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    std::optional<ScannedFileRow> ScannedFilesOps::getByPath(DatabaseManager &db, const std::string &file_path)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            std::string p = file_path;
            stmt << std::string(SQL::kSelectScannedFileByPath), Keywords::use(p), Keywords::now;
            RecordSet rs(stmt);
            if (!rs.moveFirst())
                return std::nullopt;
            ScannedFileRow r;
            r.id = rs[0].convert<int>();
            r.file_path = rs[1].convert<std::string>();
            r.relative_path = rs[2].convert<std::string>();
            r.share_name = rs[3].convert<std::string>();
            r.file_name = rs[4].convert<std::string>();
            r.file_metadata = rs[5].convert<std::string>();
            r.processed_fast = rs[6].convert<int>();
            r.processed_balanced = rs[7].convert<int>();
            r.processed_quality = rs[8].convert<int>();
            r.links_fast = rs[9].convert<std::string>();
            r.links_balanced = rs[10].convert<std::string>();
            r.links_quality = rs[11].convert<std::string>();
            r.is_network_file = rs[12].convert<int>() != 0;
            r.created_at = rs[13].convert<std::string>();
            return r;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    std::vector<ScannedFileRow> ScannedFilesOps::listAll(DatabaseManager &db)
    {
        std::vector<ScannedFileRow> out;
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            stmt << std::string(SQL::kListScannedFiles), Keywords::now;
            RecordSet rs(stmt);
            bool more = rs.moveFirst();
            while (more)
            {
                ScannedFileRow r;
                r.id = rs[0].convert<int>();
                r.file_path = rs[1].convert<std::string>();
                r.relative_path = rs[2].convert<std::string>();
                r.share_name = rs[3].convert<std::string>();
                r.file_name = rs[4].convert<std::string>();
                r.file_metadata = rs[5].convert<std::string>();
                r.processed_fast = rs[6].convert<int>();
                r.processed_balanced = rs[7].convert<int>();
                r.processed_quality = rs[8].convert<int>();
                r.links_fast = rs[9].convert<std::string>();
                r.links_balanced = rs[10].convert<std::string>();
                r.links_quality = rs[11].convert<std::string>();
                r.is_network_file = rs[12].convert<int>() != 0;
                r.created_at = rs[13].convert<std::string>();
                out.emplace_back(std::move(r));
                more = rs.moveNext();
            }
        }
        catch (...)
        {
        }
        return out;
    }

    int ScannedFilesOps::count(DatabaseManager &db)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            stmt << std::string(SQL::kCountScannedFiles), Keywords::now;
            RecordSet rs(stmt);
            if (rs.moveFirst())
            {
                return rs[0].convert<int>();
            }
        }
        catch (...)
        {
        }
        return 0;
    }

    int ScannedFilesOps::countProcessed(DatabaseManager &db)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            stmt << std::string(SQL::kCountProcessedFiles), Keywords::now;
            RecordSet rs(stmt);
            if (rs.moveFirst())
            {
                return rs[0].convert<int>();
            }
        }
        catch (...)
        {
        }
        return 0;
    }

    int ScannedFilesOps::countProcessed(DatabaseManager &db, ServerMode mode)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);

            std::string_view query;
            switch (mode)
            {
            case ServerMode::FAST:
                query = SQL::kCountProcessedFilesFast;
                break;
            case ServerMode::BALANCED:
                query = SQL::kCountProcessedFilesBalanced;
                break;
            case ServerMode::QUALITY:
                query = SQL::kCountProcessedFilesQuality;
                break;
            default:
                query = SQL::kCountProcessedFilesFast;
                break;
            }

            stmt << std::string(query), Keywords::now;
            RecordSet rs(stmt);
            if (rs.moveFirst())
            {
                return rs[0].convert<int>();
            }
        }
        catch (...)
        {
        }
        return 0;
    }

    int ScannedFilesOps::countError(DatabaseManager &db, ServerMode mode)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);

            std::string_view query;
            switch (mode)
            {
            case ServerMode::FAST:
                query = SQL::kCountErrorFilesFast;
                break;
            case ServerMode::BALANCED:
                query = SQL::kCountErrorFilesBalanced;
                break;
            case ServerMode::QUALITY:
                query = SQL::kCountErrorFilesQuality;
                break;
            default:
                query = SQL::kCountErrorFilesFast;
                break;
            }

            stmt << std::string(query), Keywords::now;
            RecordSet rs(stmt);
            if (rs.moveFirst())
            {
                return rs[0].convert<int>();
            }
        }
        catch (...)
        {
        }
        return 0;
    }

    static std::string modeToLinksColumn(ServerMode mode)
    {
        switch (mode)
        {
        case ServerMode::FAST:
            return "links_fast";
        case ServerMode::BALANCED:
            return "links_balanced";
        case ServerMode::QUALITY:
            return "links_quality";
        }
        return "links_fast";
    }

    bool ScannedFilesOps::markProcessed(DatabaseManager &db, const std::string &file_path, ServerMode mode, int state)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            int flag = state;
            std::string p = file_path;
            switch (mode)
            {
            case ServerMode::FAST:
                stmt << std::string(SQL::kUpdateProcessedFast), Keywords::use(flag), Keywords::use(p), Keywords::now;
                break;
            case ServerMode::BALANCED:
                stmt << std::string(SQL::kUpdateProcessedBalanced), Keywords::use(flag), Keywords::use(p), Keywords::now;
                break;
            case ServerMode::QUALITY:
                stmt << std::string(SQL::kUpdateProcessedQuality), Keywords::use(flag), Keywords::use(p), Keywords::now;
                break;
            }

            // Log successful update for debugging
            Poco::Logger::get("ScannedFilesOps").debug("Successfully updated file " + file_path + " to state " + std::to_string(state));
            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ScannedFilesOps").error("Failed to mark file " + file_path + " with state " + std::to_string(state) + ": " + e.what());
            return false;
        }
        catch (...)
        {
            Poco::Logger::get("ScannedFilesOps").error("Failed to mark file " + file_path + " with state " + std::to_string(state) + ": unknown error");
            return false;
        }
    }

    static std::string joinIds(const std::vector<int> &ids)
    {
        std::string s;
        for (size_t i = 0; i < ids.size(); ++i)
        {
            if (i)
                s += ",";
            s += std::to_string(ids[i]);
        }
        return s;
    }

    bool ScannedFilesOps::setLinks(DatabaseManager &db, const std::string &file_path, ServerMode mode, const std::vector<int> &link_ids)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            std::string links = joinIds(link_ids);
            std::string p = file_path;
            switch (mode)
            {
            case ServerMode::FAST:
                stmt << std::string(SQL::kUpdateLinksFast), Keywords::use(links), Keywords::use(p), Keywords::now;
                break;
            case ServerMode::BALANCED:
                stmt << std::string(SQL::kUpdateLinksBalanced), Keywords::use(links), Keywords::use(p), Keywords::now;
                break;
            case ServerMode::QUALITY:
                stmt << std::string(SQL::kUpdateLinksQuality), Keywords::use(links), Keywords::use(p), Keywords::now;
                break;
            }
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    // Overloads that infer ServerMode from config
    bool ScannedFilesOps::markProcessed(DatabaseManager &db, UnifiedObservableConfigManager &cfg,
                                        const std::string &file_path, int state)
    {
        ServerMode mode = cfg.getServerMode();
        return markProcessed(db, file_path, mode, state);
    }

    bool ScannedFilesOps::setLinks(DatabaseManager &db, UnifiedObservableConfigManager &cfg,
                                   const std::string &file_path, const std::vector<int> &link_ids)
    {
        ServerMode mode = cfg.getServerMode();
        return setLinks(db, file_path, mode, link_ids);
    }

    std::vector<int> ScannedFilesOps::getLinks(DatabaseManager &db, UnifiedObservableConfigManager &cfg,
                                               const std::string &file_path)
    {
        ServerMode mode = cfg.getServerMode();
        return getLinks(db, file_path, mode);
    }

    std::vector<ScannedFileRow> ScannedFilesOps::listUnprocessed(DatabaseManager &db, UnifiedObservableConfigManager &cfg,
                                                                 int limit)
    {
        ServerMode mode = cfg.getServerMode();
        return listUnprocessed(db, mode, limit);
    }

    static std::vector<int> splitIds(const std::string &s)
    {
        std::vector<int> out;
        size_t start = 0;
        while (start < s.size())
        {
            size_t pos = s.find(',', start);
            std::string token = (pos == std::string::npos) ? s.substr(start) : s.substr(start, pos - start);
            if (!token.empty())
            {
                try
                {
                    out.push_back(std::stoi(token));
                }
                catch (...)
                {
                }
            }
            if (pos == std::string::npos)
                break;
            start = pos + 1;
        }
        return out;
    }

    std::vector<int> ScannedFilesOps::getLinks(DatabaseManager &db, const std::string &file_path, ServerMode mode)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            std::string p = file_path;
            switch (mode)
            {
            case ServerMode::FAST:
                stmt << std::string(SQL::kSelectLinksFast), Keywords::use(p), Keywords::now;
                break;
            case ServerMode::BALANCED:
                stmt << std::string(SQL::kSelectLinksBalanced), Keywords::use(p), Keywords::now;
                break;
            case ServerMode::QUALITY:
                stmt << std::string(SQL::kSelectLinksQuality), Keywords::use(p), Keywords::now;
                break;
            }
            RecordSet rs(stmt);
            if (!rs.moveFirst())
                return {};
            std::string links = rs[0].convert<std::string>();
            return splitIds(links);
        }
        catch (...)
        {
            return {};
        }
    }

    std::vector<ScannedFileRow> ScannedFilesOps::listUnprocessed(DatabaseManager &db, ServerMode mode, int limit)
    {
        std::vector<ScannedFileRow> out;
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            switch (mode)
            {
            case ServerMode::FAST:
                stmt << std::string(SQL::kListUnprocessedFast);
                break;
            case ServerMode::BALANCED:
                stmt << std::string(SQL::kListUnprocessedBalanced);
                break;
            case ServerMode::QUALITY:
                stmt << std::string(SQL::kListUnprocessedQuality);
                break;
            }
            if (limit > 0)
            {
                stmt << " LIMIT " << limit;
            }
            stmt.execute();
            RecordSet rs(stmt);
            bool more = rs.moveFirst();
            while (more)
            {
                ScannedFileRow r;
                r.id = rs[0].convert<int>();
                r.file_path = rs[1].convert<std::string>();
                r.relative_path = rs[2].convert<std::string>();
                r.share_name = rs[3].convert<std::string>();
                r.file_name = rs[4].convert<std::string>();
                r.file_metadata = rs[5].convert<std::string>();
                r.processed_fast = rs[6].convert<int>();
                r.processed_balanced = rs[7].convert<int>();
                r.processed_quality = rs[8].convert<int>();
                r.links_fast = rs[9].convert<std::string>();
                r.links_balanced = rs[10].convert<std::string>();
                r.links_quality = rs[11].convert<std::string>();
                r.is_network_file = rs[12].convert<int>() != 0;
                r.created_at = rs[13].convert<std::string>();
                out.emplace_back(std::move(r));
                more = rs.moveNext();
            }
        }
        catch (...)
        {
        }
        return out;
    }

    bool ScannedFilesOps::updateMetadata(DatabaseManager &db, const std::string &file_path, const std::string &file_metadata)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            std::string p = file_path;
            std::string m = file_metadata;
            stmt << std::string(SQL::kUpdateMetadata), Keywords::use(m), Keywords::use(p), Keywords::now;
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    int ScannedFilesOps::clearProcessingFlags(DatabaseManager &db)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();

            // First, count how many files are currently in processing state
            Statement count_stmt(sess);
            int count = 0;
            count_stmt << std::string(SQL::kCountProcessingFiles), Keywords::into(count), Keywords::now;

            if (count == 0)
            {
                Poco::Logger::get("ScannedFilesOps").information("No files in processing state to clear");
                return 0;
            }

            // Now clear the processing flags
            Statement clear_stmt(sess);
            clear_stmt << std::string(SQL::kClearProcessingFlags), Keywords::now;

            Poco::Logger::get("ScannedFilesOps").information("Cleared processing flags for " + std::to_string(count) + " files");

            return count;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ScannedFilesOps").error("Failed to clear processing flags: " + std::string(e.what()));
            return -1; // Return -1 to indicate error
        }
    }

    bool ScannedFilesOps::fileExists(DatabaseManager &db, const std::string &file_path)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();

            int exists = 0;
            std::string file_path_copy = file_path;

            Statement stmt(sess);
            stmt << std::string(SQL::kFileExists),
                Keywords::use(file_path_copy),
                Keywords::into(exists),
                Keywords::now;

            return exists == 1;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ScannedFilesOps").error("Failed to check file existence for " + file_path + ": " + std::string(e.what()));
            return false;
        }
    }
}
