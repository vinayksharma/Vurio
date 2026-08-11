#include "database/processing_errors_ops.hpp"
#include "database/database_manager.hpp"
#include "database/db_retry.hpp"
#include "database/sql_constants.hpp"
#include "config/config_enums.hpp"
#include <Poco/Data/Session.h>
#include <Poco/Data/Statement.h>
#include <Poco/Logger.h>

namespace MediaDedup
{
    using namespace Poco::Data;

    bool ProcessingErrorsOps::ensureTable(DatabaseManager &db)
    {
        // Create table first
        if (!db.ensureTableExists("processing_errors", SQL::kCreateProcessingErrorsTable))
        {
            return false;
        }

        // Create indexes for efficient lookups
        try
        {
            auto lease = db.acquireSessionLease();
            Poco::Data::Session &sess = lease.get();
            Poco::Data::Statement stmt(sess);

            // Create file_path index
            stmt << std::string(SQL::kCreateProcessingErrorsIndexFilePath), Poco::Data::Keywords::now;
            Poco::Logger::get("ProcessingErrorsOps").debug("Created index on processing_errors.file_path");

            // Create timestamp index (for temporal queries)
            stmt << std::string(SQL::kCreateProcessingErrorsIndexTimestamp), Poco::Data::Keywords::now;
            Poco::Logger::get("ProcessingErrorsOps").debug("Created index on processing_errors.timestamp");
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ProcessingErrorsOps").warning("Failed to create indexes (may already exist): " + std::string(e.what()));
            // Don't fail - table exists, indexes are just optimizations
        }

        return true;
    }

    bool ProcessingErrorsOps::insertError(
        DatabaseManager &db,
        const std::string &file_path,
        ServerMode mode,
        int error_code,
        const std::string &error_message,
        const std::string &error_source)
    {
        // A dropped error row loses the only record of why a file failed, and
        // the escalation logic that reads it then treats the file as untried.
        return DbRetry::withBusyRetry("ProcessingErrorsOps", "insertError", [&]() {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);

            // Convert ServerMode enum to string
            std::string mode_str;
            switch (mode)
            {
            case ServerMode::EMBEDDING:
                mode_str = "EMBEDDING";
                break;
            default:
                mode_str = "UNKNOWN";
                break;
            }

            // Prepare statement with parameters
            std::string fp = file_path;
            std::string ms = mode_str;
            int ec = error_code;
            std::string em = error_message;
            std::string es = error_source;

            stmt << std::string(SQL::kInsertProcessingError),
                Keywords::use(fp),
                Keywords::use(ms),
                Keywords::use(ec),
                Keywords::use(em),
                Keywords::use(es),
                Keywords::now;

            Poco::Logger::get("ProcessingErrorsOps").debug("Inserted processing error for file: " + file_path + " (code: " + std::to_string(error_code) + ", source: " + error_source + ")");
        });
    }

    int ProcessingErrorsOps::deleteBySource(DatabaseManager &db, const std::string &error_source)
    {
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();

            std::string es = error_source;
            Statement stmt(sess);
            stmt << std::string(SQL::kDeleteProcessingErrorsBySource),
                Keywords::use(es),
                Keywords::now;

            int changes = 0;
            Statement count(sess);
            count << "SELECT changes()", Keywords::into(changes), Keywords::now;

            Poco::Logger::get("ProcessingErrorsOps").debug("Deleted " + std::to_string(changes) + " processing errors from source: " + error_source);
            return changes;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ProcessingErrorsOps").error("Failed to delete processing errors for source " + error_source + ": " + std::string(e.what()));
            return -1;
        }
    }
}
