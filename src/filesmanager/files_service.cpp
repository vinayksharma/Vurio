#include "filesmanager/files_service.hpp"
#include "database/user_settings_ops.hpp"
#include "database/sql_constants.hpp"
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
        const std::string key = makeMediaLocationKey(directory_path);
        return UserSettingsOps::upsert(db_manager_, key, directory_path);
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

} // namespace MediaDedup
