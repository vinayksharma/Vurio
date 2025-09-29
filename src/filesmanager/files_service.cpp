#include "filesmanager/files_service.hpp"
#include "database/user_settings_ops.hpp"
#include "database/sql_constants.hpp"
#include "config/unified_observable_config.hpp"
#include <Poco/DigestEngine.h>
#include <Poco/SHA1Engine.h>
#include <Poco/Path.h>
#include <algorithm>

namespace MediaDedup
{

    static std::string normalizePathForKey(const std::string &path)
    {
        std::string p = path;
        std::replace(p.begin(), p.end(), '\\', '/');
        std::transform(p.begin(), p.end(), p.begin(), [](unsigned char c)
                       { return static_cast<char>(std::tolower(c)); });
        return p;
    }

    std::string FilesService::makeMediaLocationKey(const std::string &directory_path)
    {
        std::string norm = normalizePathForKey(directory_path);
        Poco::SHA1Engine sha1;
        sha1.update(norm);
        auto digest = sha1.digest();
        std::string hex = Poco::DigestEngine::digestToHex(digest);
        return std::string("mediaLocation:") + hex;
    }

    bool FilesService::registerMediaLocation(const std::string &directory_path)
    {
        // Validate input
        if (directory_path.empty())
        {
            logger_.warning("Cannot register empty directory path");
            return false;
        }

        const std::string key = makeMediaLocationKey(directory_path);
        bool success = UserSettingsOps::upsert(db_manager_, key, directory_path);

        // Trigger immediate job execution if registration was successful, callback is set, and immediate triggering is enabled
        if (success && path_registered_callback_ && isImmediateJobTriggerEnabled())
        {
            logger_.debug("Path registered successfully, triggering immediate job execution: %s", directory_path);
            try
            {
                path_registered_callback_(directory_path);
            }
            catch (const std::exception &e)
            {
                logger_.error("Exception in path registered callback: %s", std::string(e.what()));
            }
            catch (...)
            {
                logger_.error("Unknown exception in path registered callback");
            }
        }

        return success;
    }

    bool FilesService::deregisterMediaLocation(const std::string &directory_path)
    {
        const std::string key = makeMediaLocationKey(directory_path);
        return UserSettingsOps::remove(db_manager_, key);
    }

    std::unordered_map<std::string, std::string> FilesService::listMediaLocations()
    {
        auto all = UserSettingsOps::list(db_manager_);
        std::unordered_map<std::string, std::string> out;
        for (const auto &kv : all)
        {
            if (kv.first.rfind("mediaLocation:", 0) == 0)
            {
                // Return map normalizedPath -> originalPath to match tests and API expectations
                std::string norm = normalizePathForKey(kv.second);
                out.emplace(norm, kv.second);
            }
        }
        return out;
    }

    void FilesService::setPathRegisteredCallback(std::function<void(const std::string &)> callback)
    {
        path_registered_callback_ = std::move(callback);
        logger_.debug("Path registered callback set");
    }

    bool FilesService::isImmediateJobTriggerEnabled() const
    {
        if (!config_manager_)
        {
            return true; // Default to enabled if no config manager
        }
        return config_manager_->getPropertyValue<bool>("files.service.immediateJobTrigger.enabled", true);
    }

} // namespace MediaDedup
