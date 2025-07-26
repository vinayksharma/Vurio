#include "core/mount_manager.hpp"
#include "logging/logger.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <sys/statvfs.h>
#include <sys/mount.h>

#ifdef __APPLE__
#include <sys/param.h>
#include <sys/ucred.h>
#include <sys/mount.h>
#endif

MountManager &MountManager::getInstance()
{
    static MountManager instance;
    return instance;
}

std::vector<MountInfo> MountManager::detectMounts()
{
    std::vector<MountInfo> mounts;

    // Read /proc/mounts on Linux or use mount command on macOS
#ifdef __APPLE__
    // Use mount command on macOS
    FILE *pipe = popen("mount", "r");
    if (!pipe)
    {
        Logger::error("Failed to execute mount command");
        return mounts;
    }

    char buffer[1024];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr)
    {
        std::string line(buffer);

        // Parse mount output: device on mountpoint type options
        std::istringstream iss(line);
        std::string device, mountpoint, type, options;

        if (iss >> device >> mountpoint >> type >> options)
        {
            MountInfo mount;
            mount.mount_point = mountpoint;
            mount.mount_type = type;
            mount.is_network_mount = (type == "smbfs" || type == "nfs" || type == "afpfs");

            if (mount.is_network_mount)
            {
                // Parse SMB mount info: //user@server/share on /Volumes/mountpoint
                if (device.find("//") == 0)
                {
                    // Extract server and share info
                    size_t at_pos = device.find('@');
                    size_t slash_pos = device.find('/', 2);

                    if (at_pos != std::string::npos && slash_pos != std::string::npos)
                    {
                        std::string user = device.substr(2, at_pos - 2);
                        std::string server_share = device.substr(at_pos + 1);

                        size_t share_pos = server_share.find('/');
                        if (share_pos != std::string::npos)
                        {
                            mount.server_name = server_share.substr(0, share_pos);
                            mount.share_name = server_share.substr(share_pos + 1);
                        }
                        else
                        {
                            mount.server_name = server_share;
                            mount.share_name = "";
                        }
                    }
                }
            }

            mounts.push_back(mount);
        }
    }

    pclose(pipe);
#else
    // Read /proc/mounts on Linux
    std::ifstream mounts_file("/proc/mounts");
    std::string line;

    while (std::getline(mounts_file, line))
    {
        std::istringstream iss(line);
        std::string device, mountpoint, type, options;

        if (iss >> device >> mountpoint >> type >> options)
        {
            MountInfo mount;
            mount.mount_point = mountpoint;
            mount.mount_type = type;
            mount.is_network_mount = (type == "smbfs" || type == "nfs" || type == "cifs");

            if (mount.is_network_mount)
            {
                // Parse SMB mount info
                if (device.find("//") == 0)
                {
                    size_t slash_pos = device.find('/', 2);
                    if (slash_pos != std::string::npos)
                    {
                        mount.server_name = device.substr(2, slash_pos - 2);
                        mount.share_name = device.substr(slash_pos + 1);
                    }
                }
            }

            mounts.push_back(mount);
        }
    }
#endif

    Logger::info("Detected " + std::to_string(mounts.size()) + " mounts");
    return mounts;
}

std::optional<RelativePath> MountManager::toRelativePath(const std::string &absolute_path)
{
    refreshMounts();

    auto mount_info = findMountForPath(absolute_path);
    if (!mount_info)
    {
        return std::nullopt;
    }

    // Convert to relative path
    fs::path abs_path(absolute_path);
    fs::path mount_path(mount_info->mount_point);

    try
    {
        fs::path relative = fs::relative(abs_path, mount_path);
        if (relative.empty())
        {
            return std::nullopt;
        }

        RelativePath result;
        result.share_name = mount_info->share_name;
        result.relative_path = relative.string();
        result.file_name = abs_path.filename().string();

        Logger::debug("Converted " + absolute_path + " to relative path: " +
                      result.share_name + ":" + result.relative_path);

        return result;
    }
    catch (const fs::filesystem_error &e)
    {
        Logger::warn("Failed to convert path: " + std::string(e.what()));
        return std::nullopt;
    }
}

std::optional<std::string> MountManager::toAbsolutePath(const RelativePath &relative_path)
{
    refreshMounts();

    // Find mount for this share
    for (const auto &mount : mounts_cache_)
    {
        if (mount.share_name == relative_path.share_name && mount.is_network_mount)
        {
            fs::path mount_path(mount.mount_point);
            fs::path relative_path_fs(relative_path.relative_path);
            fs::path absolute_path = mount_path / relative_path_fs;

            Logger::debug("Converted relative path " + relative_path.share_name + ":" +
                          relative_path.relative_path + " to " + absolute_path.string());

            return absolute_path.string();
        }
    }

    Logger::warn("No mount found for share: " + relative_path.share_name);
    return std::nullopt;
}

bool MountManager::isNetworkPath(const std::string &path)
{
    refreshMounts();
    return findMountForPath(path).has_value();
}

std::optional<MountInfo> MountManager::getMountInfo(const std::string &path)
{
    refreshMounts();
    return findMountForPath(path);
}

void MountManager::refreshMounts()
{
    mounts_cache_ = detectMounts();
    updateMountMap();
}

bool MountManager::validateRelativePath(const RelativePath &relative_path)
{
    auto absolute_path = toAbsolutePath(relative_path);
    if (!absolute_path)
    {
        return false;
    }

    // Check if file exists
    return fs::exists(*absolute_path);
}

void MountManager::updateMountMap()
{
    mount_map_.clear();
    for (const auto &mount : mounts_cache_)
    {
        mount_map_[mount.mount_point] = mount;
    }
}

std::optional<MountInfo> MountManager::findMountForPath(const std::string &path)
{
    fs::path file_path(path);

    // Find the longest matching mount point
    std::optional<MountInfo> best_match;
    size_t best_length = 0;

    for (const auto &mount : mounts_cache_)
    {
        if (!mount.is_network_mount)
        {
            continue;
        }

        fs::path mount_path(mount.mount_point);
        try
        {
            if (file_path.string().find(mount_path.string()) == 0)
            {
                if (mount_path.string().length() > best_length)
                {
                    best_length = mount_path.string().length();
                    best_match = mount;
                }
            }
        }
        catch (const fs::filesystem_error &)
        {
            // Skip invalid paths
            continue;
        }
    }

    return best_match;
}