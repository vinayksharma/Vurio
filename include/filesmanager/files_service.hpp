#pragma once

#include <string>
#include <unordered_map>
#include <mutex>
#include <Poco/Logger.h>

#include "database/database_manager.hpp"

namespace MediaDedup
{

    class FilesService
    {
    public:
        explicit FilesService(DatabaseManager &db_manager)
            : db_manager_(db_manager), logger_(Poco::Logger::get("FilesService")) {}

        // Media locations API (backed by user_settings table)
        bool registerMediaLocation(const std::string &directory_path);
        bool deregisterMediaLocation(const std::string &directory_path);
        std::unordered_map<std::string, std::string> listMediaLocations();

    private:
        DatabaseManager &db_manager_;
        Poco::Logger &logger_;

        static std::string makeMediaLocationKey(const std::string &directory_path);
    };

} // namespace MediaDedup
