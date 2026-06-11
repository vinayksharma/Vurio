#include "filesmanager/media_availability.hpp"

#include <algorithm>

#ifdef __APPLE__
#include <sys/param.h>
#include <sys/ucred.h>
#include <sys/mount.h>
#else
#include <fstream>
#include <sstream>
#endif

namespace MediaDedup
{
    namespace
    {
        // Normalize separators to '/' and strip a single trailing slash (keeping
        // the root "/" intact). Mirrors the normalization used elsewhere for
        // path-prefix comparisons.
        std::string normalize(const std::string &path)
        {
            std::string p = path;
            std::replace(p.begin(), p.end(), '\\', '/');
            while (p.length() > 1 && p.back() == '/')
                p.pop_back();
            return p;
        }

        // Returns the substring of `path` up to and including the Nth '/'-segment
        // after `prefix`, or empty if there are not enough segments.
        // e.g. takeSegments("/Volumes/USB/a/b", "/Volumes/", 1) -> "/Volumes/USB".
        std::string takeSegments(const std::string &path, const std::string &prefix, int segments)
        {
            size_t pos = prefix.length();
            std::string result = prefix;
            if (!result.empty() && result.back() == '/')
                result.pop_back();
            for (int i = 0; i < segments; ++i)
            {
                size_t next = path.find('/', pos);
                std::string seg = (next == std::string::npos)
                                      ? path.substr(pos)
                                      : path.substr(pos, next - pos);
                if (seg.empty())
                    return ""; // not enough segments
                result += "/" + seg;
                if (next == std::string::npos)
                {
                    pos = path.length();
                    // Fewer segments than requested but we consumed the last one.
                    return (i + 1 == segments) ? result : "";
                }
                pos = next + 1;
            }
            return result;
        }
    }

    std::vector<std::string> MediaAvailability::mountedFilesystems()
    {
        std::vector<std::string> result;
#ifdef __APPLE__
        // MNT_NOWAIT: return cached info without contacting the filesystem, so a
        // hung network mount cannot block us.
        int count = getfsstat(nullptr, 0, MNT_NOWAIT);
        if (count <= 0)
            return result;
        std::vector<struct statfs> buf(static_cast<size_t>(count));
        int n = getfsstat(buf.data(), static_cast<int>(buf.size() * sizeof(struct statfs)), MNT_NOWAIT);
        if (n <= 0)
            return result;
        result.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i)
            result.emplace_back(buf[static_cast<size_t>(i)].f_mntonname);
#else
        std::ifstream mounts("/proc/self/mounts");
        std::string line;
        while (std::getline(mounts, line))
        {
            std::istringstream iss(line);
            std::string device, mountpoint;
            if (iss >> device >> mountpoint)
                result.push_back(mountpoint);
        }
#endif
        return result;
    }

    std::string MediaAvailability::deriveVolumeRoot(const std::string &path)
    {
        const std::string p = normalize(path);

        // macOS removable / external / network volumes: /Volumes/<name>
        if (p.rfind("/Volumes/", 0) == 0)
        {
            std::string vr = takeSegments(p, "/Volumes/", 1);
            return vr.empty() ? std::string("/") : vr;
        }
        // Linux removable media typically: /media/<user>/<label> or /media/<label>
        if (p.rfind("/media/", 0) == 0)
        {
            std::string vr = takeSegments(p, "/media/", 2);
            if (vr.empty())
                vr = takeSegments(p, "/media/", 1);
            return vr.empty() ? std::string("/") : vr;
        }
        // Linux explicit mounts: /mnt/<vol>
        if (p.rfind("/mnt/", 0) == 0)
        {
            std::string vr = takeSegments(p, "/mnt/", 1);
            return vr.empty() ? std::string("/") : vr;
        }
        // Boot volume (or any path we don't treat as removable) -> always available.
        return "/";
    }

    bool MediaAvailability::isPathAvailable(const std::string &path,
                                            const std::vector<std::string> &mounts)
    {
        const std::string volume_root = deriveVolumeRoot(path);
        if (volume_root == "/")
            return true; // boot volume is always mounted

        for (const auto &m : mounts)
        {
            if (normalize(m) == volume_root)
                return true;
        }
        return false;
    }
}
