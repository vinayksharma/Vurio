#pragma once

#include <string>
#include <unordered_map>
#include <map>
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

        // Utility method for creating location keys
        static std::string makeMediaLocationKey(const std::string &directory_path);

        // Path change operation
        struct ChangePathResult
        {
            bool success;
            bool partial_success; // true if some updates succeeded but not all
            int files_verified;
            int files_verified_success;
            int total_files;
            int files_updated;
            int files_failed;
            double verification_success_rate;
            std::map<std::string, std::pair<int, int>> update_details; // table_name -> (updated, failed)
            std::string error_message;
        };

        ChangePathResult changeMediaLocationPath(
            const std::string &old_path,
            const std::string &new_path,
            int sample_size = 20);

        // Verification structure for path remapping
        struct PathRemappingVerification
        {
            bool is_successful;
            bool is_in_progress;             // True if remapping appears to be in progress (counts changing)
            int old_location_key_file_count; // Should be 0 if successful
            int new_location_key_file_count; // Should match expected count
            int files_with_old_path_prefix;  // Files still referencing old path
            int files_with_new_path_prefix;  // Files referencing new path
            int sampled_files_verified;      // Files verified to exist at new location
            int sampled_files_total;
            bool old_location_registered; // Old location still in user_settings
            bool new_location_registered; // New location in user_settings
            std::string old_location_key;
            std::string new_location_key;
            std::string error_message;
        };

        PathRemappingVerification verifyPathRemapping(
            const std::string &old_path,
            const std::string &new_path,
            int sample_size = 20);

    private:
        DatabaseManager &db_manager_;
        std::shared_ptr<UnifiedObservableConfigManager> config_manager_;
        Poco::Logger &logger_;
        std::function<void(const std::string &)> path_registered_callback_;
    };

} // namespace MediaDedup
