#pragma once

#include <string>
#include <vector>
#include <map>
#include <filesystem>
#include <optional>

namespace fs = std::filesystem;

struct MountInfo
{
    std::string share_name;  // e.g., "B", "G"
    std::string mount_point; // e.g., "/Volumes/truenas._smb._tcp.local-1"
    std::string server_name; // e.g., "truenas._smb._tcp.local"
    bool is_network_mount;   // true for SMB/NFS mounts
    std::string mount_type;  // "smbfs", "nfs", etc.
};

struct RelativePath
{
    std::string share_name;    // The share this file belongs to
    std::string relative_path; // Path relative to the share root
    std::string file_name;     // Just the filename
};

class MountManager
{
public:
    static MountManager &getInstance();

    /**
     * @brief Detect all current SMB/NFS mounts
     * @return Vector of mount information
     */
    std::vector<MountInfo> detectMounts();

    /**
     * @brief Convert absolute path to relative path with share info
     * @param absolute_path The absolute file path
     * @return Optional relative path info, or nullopt if not on a network mount
     */
    std::optional<RelativePath> toRelativePath(const std::string &absolute_path);

    /**
     * @brief Convert relative path back to absolute path
     * @param relative_path The relative path info
     * @return Optional absolute path, or nullopt if mount not found
     */
    std::optional<std::string> toAbsolutePath(const RelativePath &relative_path);

    /**
     * @brief Check if a path is on a network mount
     * @param path The path to check
     * @return true if on network mount
     */
    bool isNetworkPath(const std::string &path);

    /**
     * @brief Get mount info for a path
     * @param path The path to check
     * @return Optional mount info, or nullopt if not on a mount
     */
    std::optional<MountInfo> getMountInfo(const std::string &path);

    /**
     * @brief Refresh mount cache
     */
    void refreshMounts();

    /**
     * @brief Validate that a relative path can be resolved
     * @param relative_path The relative path to validate
     * @return true if path can be resolved
     */
    bool validateRelativePath(const RelativePath &relative_path);

private:
    MountManager() = default;
    ~MountManager() = default;
    MountManager(const MountManager &) = delete;
    MountManager &operator=(const MountManager &) = delete;

    std::vector<MountInfo> mounts_cache_;
    std::map<std::string, MountInfo> mount_map_; // mount_point -> MountInfo

    void updateMountMap();
    std::optional<MountInfo> findMountForPath(const std::string &path);
};