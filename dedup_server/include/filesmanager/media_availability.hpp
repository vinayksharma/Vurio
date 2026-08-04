#pragma once

#include <string>
#include <vector>

namespace MediaDedup
{
    // Lightweight, non-blocking media availability detection based on the OS
    // mount table. It never stats individual file paths, so it cannot hang on a
    // disconnected network mount and is cheap regardless of how many files are
    // tracked. Availability is determined per *volume*: a file is available iff
    // the volume it resides on is currently mounted.
    class MediaAvailability
    {
    public:
        // Mount points of all currently-mounted filesystems (e.g. "/",
        // "/Volumes/USB"), separator-normalized with trailing slashes stripped
        // (MP-5: normalized once here instead of per lookup). Reads the
        // in-memory mount table only; safe to call concurrently from multiple
        // threads.
        static std::vector<std::string> mountedFilesystems();

        // Derives the "volume root" a path depends on: the removable mount point
        // under which the path lives ("/Volumes/<name>" on macOS, "/media/<user>/
        // <vol>" or "/mnt/<vol>" on Linux), or "/" when the path is on the boot
        // volume. Pure string analysis; no filesystem access.
        static std::string deriveVolumeRoot(const std::string &path);

        // True if the volume `path` resides on is currently mounted. Paths on the
        // boot volume are always available. `mounts` should come from
        // mountedFilesystems(); pass it in to compute availability for many paths
        // without re-reading the mount table each time.
        static bool isPathAvailable(const std::string &path,
                                    const std::vector<std::string> &mounts);
    };
}
