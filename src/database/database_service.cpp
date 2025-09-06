#include "database/database_service.hpp"

namespace MediaDedup
{

    bool DatabaseService::ensureDatabaseFileExists()
    {
        try
        {
            // Resolve database path from configuration
            std::string default_path = "data/dedup_server.db";
            database_path_ = config_manager_ ? config_manager_->getPropertyValue<std::string>("database.path", default_path)
                                             : default_path;

            // Ensure parent directory exists
            auto db_path = std::filesystem::path(database_path_);
            auto parent = db_path.parent_path();
            if (!parent.empty() && !std::filesystem::exists(parent))
            {
                std::filesystem::create_directories(parent);
            }

            // Create file if missing
            if (!std::filesystem::exists(db_path))
            {
                std::ofstream ofs(database_path_, std::ios::binary);
                if (!ofs.is_open())
                {
                    logger_.error("Failed to create database file: " + database_path_);
                    return false;
                }
                ofs.close();
                logger_.information("Created database file: " + database_path_);
            }

            return true;
        }
        catch (const std::exception &e)
        {
            logger_.error(std::string("ensureDatabaseFileExists failed: ") + e.what());
            return false;
        }
    }

} // namespace MediaDedup

