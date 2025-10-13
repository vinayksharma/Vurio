#include "database/processing_errors_ops.hpp"
#include "database/database_manager.hpp"
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
        try
        {
            auto lease = db.acquireSessionLease();
            Session &sess = lease.get();
            Statement stmt(sess);

            // Convert ServerMode enum to string
            std::string mode_str;
            switch (mode)
            {
            case ServerMode::FAST:
                mode_str = "FAST";
                break;
            case ServerMode::BALANCED:
                mode_str = "BALANCED";
                break;
            case ServerMode::QUALITY:
                mode_str = "QUALITY";
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

            return true;
        }
        catch (const std::exception &e)
        {
            Poco::Logger::get("ProcessingErrorsOps").error("Failed to insert processing error for file " + file_path + ": " + std::string(e.what()));
            return false;
        }
        catch (...)
        {
            Poco::Logger::get("ProcessingErrorsOps").error("Failed to insert processing error for file " + file_path + ": unknown error");
            return false;
        }
    }
}
