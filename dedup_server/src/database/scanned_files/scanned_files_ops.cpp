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

        // Create indexes for efficient lookups (critical for FilesManager performance)
        // Note: UNIQUE constraint on file_path already creates an implicit index, but we add explicit one for clarity
        try
        {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            Poco::Data::Statement stmt(sess);

            // Create file_path index
            stmt << std::string(SQL::kCreateScannedFilesIndexFilePath), Poco::Data::Keywords::now;
            Poco::Logger::get("ScannedFilesOps").debug("Created index on scanned_files.file_path");

            // Create composite index for filtered queries
            stmt << std::string(SQL::kCreateScannedFilesIndexLocationProcessed), Poco::Data::Keywords::now;
            Poco::Logger::get("ScannedFilesOps").debug("Created composite index for location_key filtering");
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ScannedFilesOps").warning("Failed to create indexes (may already exist): " + std::string(e.what()));
            // Don't fail - table exists, indexes are just optimizations
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
        int p = r.processed; // Preserve state code (0,1,2,...)
        std::string l = r.links;
        int inf = r.is_network_file ? 1 : 0;
        std::string location_key = r.location_key;
        stmt << std::string(SQL::kUpsertScannedFile),
            Keywords::use(file_path),
            Keywords::use(relative_path),
            Keywords::use(share_name),
            Keywords::use(file_name),
            Keywords::use(file_metadata),
            Keywords::use(p),
            Keywords::use(l),
            Keywords::use(inf),
            Keywords::use(location_key),
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
            r.processed = rs[6].convert<int>();
            r.links = rs[7].convert<std::string>();
            r.is_network_file = rs[8].convert<int>() != 0;
            r.location_key = rs[9].convert<std::string>();
            r.created_at = rs[10].convert<std::string>();
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
                r.processed = rs[6].convert<int>();
                r.links = rs[7].convert<std::string>();
                r.is_network_file = rs[8].convert<int>() != 0;
                r.location_key = rs[9].convert<std::string>();
                r.created_at = rs[10].convert<std::string>();
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
        return executeFilteredCount(db, std::string(SQL::kCountScannedFilesFiltered));
    }

    int ScannedFilesOps::countAll(DatabaseManager &db)
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
            // Return 0 on error
        }
        return 0;
    }

    int ScannedFilesOps::countProcessed(DatabaseManager &db)
    {
        return executeFilteredCount(db, std::string(SQL::kCountProcessedFilesFiltered));
    }

    int ScannedFilesOps::countProcessedAll(DatabaseManager &db)
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
            // Return 0 on error
        }
        return 0;
    }

    // REMOVED: Mode-specific overloads - only EMBEDDING mode supported
    // REMOVED: countErrorAll, countQueuedAll, modeToLinksColumn - mode-specific methods not needed

    bool ScannedFilesOps::markProcessed(DatabaseManager &db, const std::string &file_path, int state)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            int flag = state;
            std::string p = file_path;
            stmt << std::string(SQL::kUpdateProcessed), Keywords::use(flag), Keywords::use(p), Keywords::now;

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

    bool ScannedFilesOps::markProcessedWithEscalation(DatabaseManager &db, const std::string &file_path, int state)
    {
        try
        {
            // Get current status of the file
            auto current_file = getByPath(db, file_path);
            if (!current_file.has_value())
            {
                Poco::Logger::get("ScannedFilesOps").error("File not found in database for escalation: " + file_path);
                return false;
            }

            // Get current status
            int current_status = current_file->processed;

            // Determine final status based on escalation logic
            int final_status = state;

            // If current status is already an error (< 0) and we're setting another error, escalate
            if (current_status < 0 && state < 0)
            {
                // Escalate by subtracting 100 (e.g., -1 becomes -101, -6 becomes -106)
                final_status = state - 100;
                Poco::Logger::get("ScannedFilesOps").information("Escalating error for file " + file_path + " from " + std::to_string(current_status) + " to " + std::to_string(final_status) + " (original error: " + std::to_string(state) + ")");
            }
            else if (current_status < 0)
            {
                // If current status is error but new state is success, use success state
                final_status = state;
                Poco::Logger::get("ScannedFilesOps").information("File " + file_path + " recovered from error " + std::to_string(current_status) + " to success state " + std::to_string(state));
            }

            // Use the existing markProcessed method with the final status
            return markProcessed(db, file_path, final_status);
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ScannedFilesOps").error("Failed to mark file with escalation " + file_path + " with state " + std::to_string(state) + ": " + e.what());
            return false;
        }
        catch (...)
        {
            Poco::Logger::get("ScannedFilesOps").error("Failed to mark file with escalation " + file_path + " with state " + std::to_string(state) + ": unknown error");
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

    bool ScannedFilesOps::setLinks(DatabaseManager &db, const std::string &file_path, const std::vector<int> &link_ids)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            std::string links = joinIds(link_ids);
            std::string p = file_path;
            stmt << std::string(SQL::kUpdateLinks), Keywords::use(links), Keywords::use(p), Keywords::now;
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    // REMOVED: Overloads that infer ServerMode from config - no longer needed

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

    std::vector<int> ScannedFilesOps::getLinks(DatabaseManager &db, const std::string &file_path)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            std::string p = file_path;
            stmt << std::string(SQL::kSelectLinks), Keywords::use(p), Keywords::now;
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

    std::vector<ScannedFileRow> ScannedFilesOps::listUnprocessed(DatabaseManager &db, int limit)
    {
        std::vector<ScannedFileRow> out;
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            stmt << std::string(SQL::kListUnprocessed);
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
                r.processed = rs[6].convert<int>();
                r.links = rs[7].convert<std::string>();
                r.is_network_file = rs[8].convert<int>() != 0;
                r.location_key = rs[9].convert<std::string>();
                r.created_at = rs[10].convert<std::string>();
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

    int ScannedFilesOps::resetAllErrors(DatabaseManager &db)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();

            // First count the error files
            int count = countError(db);
            if (count == 0)
            {
                Poco::Logger::get("ScannedFilesOps").information("No error files to reset");
                return 0;
            }

            // Now reset the errors
            Statement stmt(sess);
            stmt << std::string(SQL::kResetAllErrors), Keywords::now;

            Poco::Logger::get("ScannedFilesOps").information("Reset " + std::to_string(count) + " error files to unprocessed");
            return count;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ScannedFilesOps").error("Failed to reset all errors: " + std::string(e.what()));
            return -1;
        }
    }

    std::vector<std::string> ScannedFilesOps::getRegisteredLocationKeys(DatabaseManager &db)
    {
        std::vector<std::string> result;
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            std::string query = std::string(SQL::kGetRegisteredLocationKeys);
            Poco::Logger::get("ScannedFilesOps").information("Executing query: " + query);
            stmt << query, Keywords::now;
            RecordSet rs(stmt);
            bool more = rs.moveFirst();
            while (more)
            {
                std::string key = rs[0].convert<std::string>();
                Poco::Logger::get("ScannedFilesOps").information("Found key: " + key);
                result.emplace_back(std::move(key));
                more = rs.moveNext();
            }
            Poco::Logger::get("ScannedFilesOps").information("Total keys found: " + std::to_string(result.size()));
        }
        catch (const std::exception &e)
        {
            // Log the error for debugging
            Poco::Logger::get("ScannedFilesOps").error("Error in getRegisteredLocationKeys: " + std::string(e.what()));
        }
        catch (...)
        {
            // Return empty vector on error
            Poco::Logger::get("ScannedFilesOps").error("Unknown error in getRegisteredLocationKeys");
        }
        return result;
    }

    std::string ScannedFilesOps::getLocationKey(DatabaseManager &db, const std::string &file_path)
    {
        auto file_record = getByPath(db, file_path);
        if (file_record.has_value())
        {
            return file_record->location_key;
        }
        return "";
    }

    int ScannedFilesOps::executeFilteredCount(DatabaseManager &db, const std::string &base_query)
    {
        try
        {
            auto registered_keys = ScannedFilesOps::getRegisteredLocationKeys(db);
            Poco::Logger::get("ScannedFilesOps").information("executeFilteredCount: found " + std::to_string(registered_keys.size()) + " registered keys");
            if (registered_keys.empty())
            {
                return 0; // No registered locations = no files to count
            }

            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);

            // Build IN clause for location_key filtering
            std::string in_clause = "";
            for (size_t i = 0; i < registered_keys.size(); ++i)
            {
                if (i > 0)
                    in_clause += ",";
                in_clause += "'" + registered_keys[i] + "'";
            }

            std::string query = base_query;
            // Replace ? with the IN clause
            size_t pos = query.find("?");
            if (pos != std::string::npos)
            {
                query.replace(pos, 1, in_clause);
            }

            stmt << query, Keywords::now;
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

    int ScannedFilesOps::countError(DatabaseManager &db)
    {
        return executeFilteredCount(db, std::string(SQL::kCountErrorFiles));
    }

    int ScannedFilesOps::countQueued(DatabaseManager &db)
    {
        return executeFilteredCount(db, std::string(SQL::kCountQueuedFiles));
    }

    int ScannedFilesOps::countErrorAll(DatabaseManager &db)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            int count = 0;
            stmt << std::string(SQL::kCountErrorFiles), Keywords::into(count), Keywords::now;
            return count;
        }
        catch (...)
        {
            return 0;
        }
    }

    int ScannedFilesOps::countQueuedAll(DatabaseManager &db)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            int count = 0;
            stmt << std::string(SQL::kCountQueuedFiles), Keywords::into(count), Keywords::now;
            return count;
        }
        catch (...)
        {
            return 0;
        }
    }

    int ScannedFilesOps::countFilesByLocationKey(DatabaseManager &db, const std::string &location_key)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            std::string key = location_key;
            stmt << std::string(SQL::kCountScannedFilesByLocationKey), Keywords::use(key), Keywords::now;
            RecordSet rs(stmt);
            if (rs.moveFirst())
            {
                return rs[0].convert<int>();
            }
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ScannedFilesOps").error("Error in countFilesByLocationKey: %s", std::string(e.what()));
        }
        catch (...)
        {
            Poco::Logger::get("ScannedFilesOps").error("Unknown error in countFilesByLocationKey");
        }
        return 0;
    }

    std::vector<ScannedFileRow> ScannedFilesOps::getRandomFilesByLocationKey(DatabaseManager &db, const std::string &location_key, int limit)
    {
        std::vector<ScannedFileRow> out;
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            std::string key = location_key;
            int lim = limit;
            stmt << std::string(SQL::kSelectRandomScannedFilesByLocationKey), Keywords::use(key), Keywords::use(lim), Keywords::now;
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
                r.processed = rs[6].convert<int>();
                r.links = rs[7].convert<std::string>();
                r.is_network_file = rs[8].convert<int>() != 0;
                r.location_key = rs[9].convert<std::string>();
                r.created_at = rs[10].convert<std::string>();
                out.emplace_back(std::move(r));
                more = rs.moveNext();
            }
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ScannedFilesOps").error("Error in getRandomFilesByLocationKey: %s", std::string(e.what()));
        }
        catch (...)
        {
            Poco::Logger::get("ScannedFilesOps").error("Unknown error in getRandomFilesByLocationKey");
        }
        return out;
    }

    std::vector<ScannedFileRow> ScannedFilesOps::getFilesByLocationKeyBatch(DatabaseManager &db, const std::string &location_key, int limit, int offset)
    {
        std::vector<ScannedFileRow> out;
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            std::string key = location_key;
            int lim = limit;
            int off = offset;
            stmt << std::string(SQL::kSelectScannedFilesByLocationKeyWithLimit), Keywords::use(key), Keywords::use(lim), Keywords::use(off), Keywords::now;
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
                r.processed = rs[6].convert<int>();
                r.links = rs[7].convert<std::string>();
                r.is_network_file = rs[8].convert<int>() != 0;
                r.location_key = rs[9].convert<std::string>();
                r.created_at = rs[10].convert<std::string>();
                out.emplace_back(std::move(r));
                more = rs.moveNext();
            }
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ScannedFilesOps").error("Error in getFilesByLocationKeyBatch: %s", std::string(e.what()));
        }
        catch (...)
        {
            Poco::Logger::get("ScannedFilesOps").error("Unknown error in getFilesByLocationKeyBatch");
        }
        return out;
    }

    std::vector<ScannedFileRow> ScannedFilesOps::getFilesByLocationKey(DatabaseManager &db, const std::string &location_key)
    {
        Poco::Logger::get("ScannedFilesOps").information("getFilesByLocationKey: Starting query for location_key=%s", location_key);
        std::vector<ScannedFileRow> out;
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            std::string key = location_key;
            stmt << std::string(SQL::kSelectScannedFilesByLocationKey), Keywords::use(key), Keywords::now;
            RecordSet rs(stmt);
            bool more = rs.moveFirst();
            int count = 0;
            while (more)
            {
                ScannedFileRow r;
                r.id = rs[0].convert<int>();
                r.file_path = rs[1].convert<std::string>();
                r.relative_path = rs[2].convert<std::string>();
                r.share_name = rs[3].convert<std::string>();
                r.file_name = rs[4].convert<std::string>();
                r.file_metadata = rs[5].convert<std::string>();
                r.processed = rs[6].convert<int>();
                r.links = rs[7].convert<std::string>();
                r.is_network_file = rs[8].convert<int>() != 0;
                r.location_key = rs[9].convert<std::string>();
                r.created_at = rs[10].convert<std::string>();
                out.emplace_back(std::move(r));
                count++;
                if (count % 10000 == 0)
                {
                    Poco::Logger::get("ScannedFilesOps").information("getFilesByLocationKey: Loaded %d files so far", count);
                }
                more = rs.moveNext();
            }
            Poco::Logger::get("ScannedFilesOps").information("getFilesByLocationKey: Completed, loaded %d files", count);
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ScannedFilesOps").error("Error in getFilesByLocationKey: " + std::string(e.what()));
        }
        catch (...)
        {
            Poco::Logger::get("ScannedFilesOps").error("Unknown error in getFilesByLocationKey");
        }
        return out;
    }

    bool ScannedFilesOps::updateFilePath(DatabaseManager &db,
                                         int file_id,
                                         const std::string &new_path,
                                         const std::string &new_relative_path,
                                         const std::string &new_location_key)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            std::string new_p = new_path;
            std::string new_rel = new_relative_path;
            std::string new_key = new_location_key;
            int id = file_id;
            stmt << std::string(SQL::kUpdateScannedFilePath),
                Keywords::use(new_p),
                Keywords::use(new_rel),
                Keywords::use(new_key),
                Keywords::use(id),
                Keywords::now;
            int rows_affected = stmt.execute();
            if (rows_affected == 0)
            {
                Poco::Logger::get("ScannedFilesOps").warning("No rows updated for file id: %d (file may not exist in database)", file_id);
                return false;
            }
            return true;
        }
        catch (const std::exception &e)
        {
            std::string error_msg = e.what();
            // Check if it's a constraint violation (UNIQUE constraint on file_path)
            if (error_msg.find("UNIQUE") != std::string::npos ||
                error_msg.find("constraint") != std::string::npos ||
                error_msg.find("Constraint violation") != std::string::npos)
            {
                Poco::Logger::get("ScannedFilesOps").warning("Cannot update file path (id=%d) to %s: path already exists (UNIQUE constraint). "
                                                             "This may indicate duplicate files.",
                                                             file_id, new_path);
            }
            else
            {
                Poco::Logger::get("ScannedFilesOps").error("Error updating file path in scanned_files: " + error_msg);
            }
            return false;
        }
        catch (...)
        {
            Poco::Logger::get("ScannedFilesOps").error("Unknown error updating file path in scanned_files");
            return false;
        }
    }

    int ScannedFilesOps::updateFilePathInImageArtifacts(DatabaseManager &db,
                                                        const std::string &old_path,
                                                        const std::string &new_path)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            std::string new_p = new_path;
            std::string old_p = old_path;
            stmt << std::string(SQL::kUpdateImageArtifactsFilePath),
                Keywords::use(new_p),
                Keywords::use(old_p);
            return stmt.execute();
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ScannedFilesOps").error("Error updating file path in image_artifacts: " + std::string(e.what()));
            return 0;
        }
        catch (...)
        {
            Poco::Logger::get("ScannedFilesOps").error("Unknown error updating file path in image_artifacts");
            return 0;
        }
    }

    int ScannedFilesOps::updateFilePathInProcessingErrors(DatabaseManager &db,
                                                          const std::string &old_path,
                                                          const std::string &new_path)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            std::string new_p = new_path;
            std::string old_p = old_path;
            stmt << std::string(SQL::kUpdateProcessingErrorsFilePath),
                Keywords::use(new_p),
                Keywords::use(old_p);
            return stmt.execute();
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ScannedFilesOps").error("Error updating file path in processing_errors: " + std::string(e.what()));
            return 0;
        }
        catch (...)
        {
            Poco::Logger::get("ScannedFilesOps").error("Unknown error updating file path in processing_errors");
            return 0;
        }
    }

    int ScannedFilesOps::updateFilePathInDuplicateGroups(DatabaseManager &db,
                                                         const std::string &old_path,
                                                         const std::string &new_path)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            std::string new_p = new_path;
            std::string old_p = old_path;
            stmt << std::string(SQL::kUpdateDuplicateGroupsFilePath),
                Keywords::use(new_p),
                Keywords::use(old_p);
            return stmt.execute();
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ScannedFilesOps").error("Error updating file path in duplicate_groups: " + std::string(e.what()));
            return 0;
        }
        catch (...)
        {
            Poco::Logger::get("ScannedFilesOps").error("Unknown error updating file path in duplicate_groups");
            return 0;
        }
    }

    int ScannedFilesOps::updateFilePathInDuplicateMembers(DatabaseManager &db,
                                                          const std::string &old_path,
                                                          const std::string &new_path)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            std::string new_p = new_path;
            std::string old_p = old_path;
            stmt << std::string(SQL::kUpdateDuplicateMembersFilePath),
                Keywords::use(new_p),
                Keywords::use(old_p);
            return stmt.execute();
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ScannedFilesOps").error("Error updating file path in duplicate_members: " + std::string(e.what()));
            return 0;
        }
        catch (...)
        {
            Poco::Logger::get("ScannedFilesOps").error("Unknown error updating file path in duplicate_members");
            return 0;
        }
    }

    int ScannedFilesOps::updateFilePathInThumbnailCache(DatabaseManager &db,
                                                        const std::string &old_path,
                                                        const std::string &new_path)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);
            std::string new_p = new_path;
            std::string old_p = old_path;
            stmt << std::string(SQL::kUpdateThumbnailCacheSourcePath),
                Keywords::use(new_p),
                Keywords::use(old_p);
            return stmt.execute();
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ScannedFilesOps").error("Error updating file path in thumbnail_cache: " + std::string(e.what()));
            return 0;
        }
        catch (...)
        {
            Poco::Logger::get("ScannedFilesOps").error("Unknown error updating file path in thumbnail_cache");
            return 0;
        }
    }
}
