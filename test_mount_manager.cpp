#include "core/mount_manager.hpp"
#include "logging/logger.hpp"
#include <iostream>

int main()
{
    auto &mount_manager = MountManager::getInstance();

    std::cout << "=== Mount Manager Test ===" << std::endl;

    // Detect current mounts
    auto mounts = mount_manager.detectMounts();
    std::cout << "Detected " << mounts.size() << " mounts:" << std::endl;

    for (const auto &mount : mounts)
    {
        if (mount.is_network_mount)
        {
            std::cout << "  Network: " << mount.share_name << " -> " << mount.mount_point
                      << " (type: " << mount.mount_type << ")" << std::endl;
        }
    }

    // Test path conversion
    std::string test_path = "/Volumes/truenas._smb._tcp.local-1/Video/HDC-TM90/07-18-2011/07-18-2011_232237.m2ts";

    std::cout << "\nTesting path: " << test_path << std::endl;

    if (mount_manager.isNetworkPath(test_path))
    {
        std::cout << "✓ Path is on network mount" << std::endl;

        auto relative = mount_manager.toRelativePath(test_path);
        if (relative)
        {
            std::cout << "✓ Converted to relative path: " << relative->share_name << ":" << relative->relative_path << std::endl;

            auto absolute = mount_manager.toAbsolutePath(*relative);
            if (absolute)
            {
                std::cout << "✓ Converted back to absolute: " << *absolute << std::endl;

                if (*absolute == test_path)
                {
                    std::cout << "✓ Path conversion is reversible!" << std::endl;
                }
                else
                {
                    std::cout << "✗ Path conversion is not reversible" << std::endl;
                }
            }
            else
            {
                std::cout << "✗ Failed to convert back to absolute" << std::endl;
            }
        }
        else
        {
            std::cout << "✗ Failed to convert to relative path" << std::endl;
        }
    }
    else
    {
        std::cout << "✗ Path is not on network mount" << std::endl;
    }

    return 0;
}