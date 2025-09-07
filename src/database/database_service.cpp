#include "database/database_service.hpp"
#include <yaml-cpp/yaml.h>

namespace MediaDedup
{

    bool DatabaseService::ensureDatabaseFileExists()
    {
        try
        {
            // Resolve database path from configuration (fallback to YAML read if property not present)
            std::string default_path = "data/dedup_server.db";
            database_path_ = default_path;
            if (config_manager_)
            {
                // Prefer YAML file value to match tests that write config directly
                bool set_from_yaml = false;
                try
                {
                    YAML::Node cfg = YAML::LoadFile(config_manager_->getConfigFilePath());
                    if (cfg["database.path"])
                    {
                        database_path_ = cfg["database.path"].as<std::string>();
                        set_from_yaml = true;
                    }
                    else if (cfg["database"] && cfg["database"]["path"])
                    {
                        database_path_ = cfg["database"]["path"].as<std::string>();
                        set_from_yaml = true;
                    }
                }
                catch (...)
                {
                }

                if (!set_from_yaml)
                {
                    // Fallback to simple text scan to handle minimal YAML
                    try
                    {
                        std::ifstream in(config_manager_->getConfigFilePath());
                        std::string line;
                        while (std::getline(in, line))
                        {
                            auto start = line.find_first_not_of(" \t");
                            if (start == std::string::npos)
                                continue;
                            std::string s = line.substr(start);
                            const std::string key = "database.path:";
                            if (s.rfind(key, 0) == 0)
                            {
                                std::string val = s.substr(key.size());
                                auto vs = val.find_first_not_of(" \t\"'");
                                auto ve = val.find_last_not_of(" \t\"'");
                                if (vs != std::string::npos && ve != std::string::npos && ve >= vs)
                                    database_path_ = val.substr(vs, ve - vs + 1);
                                set_from_yaml = true;
                                break;
                            }
                        }
                    }
                    catch (...)
                    {
                    }

                    if (!set_from_yaml)
                    {
                        // Fallback to property or default
                        database_path_ = config_manager_->getPropertyValue<std::string>("database.path", default_path);
                    }
                }
            }

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
