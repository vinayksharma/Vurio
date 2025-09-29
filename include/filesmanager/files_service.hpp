#pragma once

#include <string>
#include <unordered_map>
#include <mutex>
#include <functional>
#include <memory>
#include <Poco/Logger.h>

#include "database/database_manager.hpp"

// Forward declaration
namespace MediaDedup
{
    class UnifiedObservableConfigManager;
}

namespace MediaDedup
{

    class FilesService
    {
    public:
        explicit FilesService(DatabaseManager &db_manager, std::shared_ptr<UnifiedObservableConfigManager> config_manager = nullptr)
            : db_manager_(db_manager), config_manager_(config_manager), logger_(Poco::Logger::get("FilesService")) {}

        // Media locations API (backed by user_settings table)
        bool registerMediaLocation(const std::string &directory_path);
        bool deregisterMediaLocation(const std::string &directory_path);
        std::unordered_map<std::string, std::string> listMediaLocations();

        // Callback mechanism for immediate job triggering
        void setPathRegisteredCallback(std::function<void(const std::string &)> callback);
        bool isImmediateJobTriggerEnabled() const;

    private:
        DatabaseManager &db_manager_;
        std::shared_ptr<UnifiedObservableConfigManager> config_manager_;
        Poco::Logger &logger_;
        std::function<void(const std::string &)> path_registered_callback_;

        static std::string makeMediaLocationKey(const std::string &directory_path);
    };

} // namespace MediaDedup
