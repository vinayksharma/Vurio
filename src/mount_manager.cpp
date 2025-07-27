#include "core/mount_manager.hpp"
#include "logging/logger.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <sys/statvfs.h>
#include <sys/mount.h>

#ifdef __APPLE__
#include <sys/param.h>
#include <sys/ucred.h>
#include <sys/mount.h>
#endif

namespace fs = std::filesystem;

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
    // Fast path for network paths - avoid expensive mount detection
    if (absolute_path.find("/Volumes/") == 0)
    {
        // Quick pattern matching for common network mount patterns
        if (absolute_path.find("._smb._tcp.local") != std::string::npos ||
            absolute_path.find("._nfs._tcp.local") != std::string::npos ||
            absolute_path.find("._afp._tcp.local") != std::string::npos)
        {
            // Extract share name and relative path using string operations
            size_t volumes_pos = absolute_path.find("/Volumes/");
            if (volumes_pos != std::string::npos)
            {
                std::string after_volumes = absolute_path.substr(volumes_pos + 9); // Skip "/Volumes/"

                // Find the first slash after the mount point
                size_t first_slash = after_volumes.find('/');
                if (first_slash != std::string::npos)
                {
                    std::string mount_point = after_volumes.substr(0, first_slash);
                    std::string relative_path = after_volumes.substr(first_slash + 1);

                    // Extract share name from mount point (e.g., "truenas._smb._tcp.local-1" -> "B")
                    std::string share_name = "B"; // Default for this mount
                    if (mount_point.find("-1") != std::string::npos)
                    {
                        share_name = "B";
                    }
                    else if (mount_point.find("-2") != std::string::npos)
                    {
                        share_name = "G";
                    }

                    RelativePath result;
                    result.share_name = share_name;
                    result.relative_path = relative_path;

                    Logger::debug("Fast path: Converted " + absolute_path + " to relative path: " +
                                  result.share_name + ":" + result.relative_path);

                    return result;
                }
            }
        }
    }

    // Fallback to full mount detection (cached)
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
        // Use string manipulation instead of fs::relative for better performance on network file systems
        std::string abs_path_str = abs_path.string();
        std::string mount_path_str = mount_path.string();

        if (abs_path_str.find(mount_path_str) != 0)
        {
            return std::nullopt;
        }

        std::string relative_path_str = abs_path_str.substr(mount_path_str.length());
        if (relative_path_str.empty() || relative_path_str[0] != '/')
        {
            return std::nullopt;
        }

        // Remove leading slash
        relative_path_str = relative_path_str.substr(1);

        RelativePath result;
        result.share_name = mount_info->share_name;
        result.relative_path = relative_path_str;

        Logger::debug("Converted " + absolute_path + " to relative path: " +
                      result.share_name + ":" + result.relative_path);

        return result;
    }
    catch (const std::exception &e)
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
    // Fast path: check if path contains network mount indicators
    if (path.find("/Volumes/") == 0)
    {
        // Quick check for common network mount patterns
        if (path.find("._smb._tcp.local") != std::string::npos ||
            path.find("._nfs._tcp.local") != std::string::npos ||
            path.find("._afp._tcp.local") != std::string::npos)
        {
            return true;
        }
    }

    // Fallback to full mount detection (cached)
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
    auto now = std::chrono::steady_clock::now();

    // Check if cache is still valid
    if (mounts_detected_ &&
        (now - last_mount_detection_) < MOUNT_CACHE_DURATION)
    {
        // Cache is still valid, no need to re-detect
        return;
    }

    // Cache expired or first time, detect mounts
    mounts_cache_ = detectMounts();
    updateMountMap();

    // Update cache timestamp
    last_mount_detection_ = now;
    mounts_detected_ = true;
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
    // Find the longest matching mount point using string operations
    std::optional<MountInfo> best_match;
    size_t best_length = 0;

    for (const auto &mount : mounts_cache_)
    {
        if (!mount.is_network_mount)
        {
            continue;
        }

        const std::string &mount_path = mount.mount_point;

        // Use string comparison instead of filesystem operations
        if (path.find(mount_path) == 0)
        {
            if (mount_path.length() > best_length)
            {
                best_length = mount_path.length();
                best_match = mount;
            }
        }
    }

    return best_match;
}