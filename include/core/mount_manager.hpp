#pragma once

#include <string>
#include <vector>
#include <optional>
#include <chrono>
#include <map>

struct MountInfo
{
    std::string mount_point;
    std::string mount_type;
    std::string server_name;
    std::string share_name;
    bool is_network_mount;
};

struct RelativePath
{
    std::string share_name;
    std::string relative_path;
};

class MountManager
{
public:
    static MountManager &getInstance();

    // Mount detection
    std::vector<MountInfo> detectMounts();
    void refreshMounts();

    // Path conversion
    std::optional<RelativePath> toRelativePath(const std::string &absolute_path);
    std::optional<std::string> toAbsolutePath(const RelativePath &relative_path);

    // Network path detection
    bool isNetworkPath(const std::string &path);
    std::optional<MountInfo> getMountInfo(const std::string &path);
    std::optional<MountInfo> findMountForPath(const std::string &path);

    // Validation
    bool validateRelativePath(const RelativePath &relative_path);

private:
    MountManager() = default;
    ~MountManager() = default;
    MountManager(const MountManager &) = delete;
    MountManager &operator=(const MountManager &) = delete;

    void updateMountMap();

    std::vector<MountInfo> mounts_cache_;
    std::map<std::string, MountInfo> mount_map_;

    // Caching fields for Option 1
    std::chrono::steady_clock::time_point last_mount_detection_;
    static constexpr std::chrono::seconds MOUNT_CACHE_DURATION{30}; // Cache for 30 seconds
    bool mounts_detected_ = false;
};