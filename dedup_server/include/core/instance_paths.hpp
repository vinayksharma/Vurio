#pragma once

// BROKER-1: where a server instance keeps its private rendezvous files.
//
//   ~/.vurio/instances/<pid>/grpc.sock    the instance's API socket
//   ~/.vurio/instances/<pid>/grpc.cookie  its pairing cookie (0600)
//
// Keyed by pid so concurrent instances never collide, and so a leftover
// directory can be recognised as stale (its pid is not alive) and removed.

#include <Poco/Logger.h>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace MediaDedup {
namespace Core {
namespace InstancePaths {

inline std::filesystem::path root() {
    const char *home = std::getenv("HOME");
    if (!home || !*home) return {};
    return std::filesystem::path(home) / ".vurio" / "instances";
}

inline std::filesystem::path forPid(pid_t pid) {
    auto r = root();
    if (r.empty()) return {};
    return r / std::to_string(static_cast<long>(pid));
}

inline bool processAlive(pid_t pid) {
    if (pid <= 0) return false;
    if (::kill(pid, 0) == 0) return true;
    return errno == EPERM;
}

// Remove instance directories whose process no longer exists — a crashed or
// SIGKILLed server never got to delete its own. Never touches a live one.
inline void pruneStale() {
    auto r = root();
    std::error_code ec;
    if (r.empty() || !std::filesystem::is_directory(r, ec)) return;
    for (const auto &entry : std::filesystem::directory_iterator(r, ec)) {
        if (!entry.is_directory(ec)) continue;
        const std::string name = entry.path().filename().string();
        char *end = nullptr;
        long pid = std::strtol(name.c_str(), &end, 10);
        if (end == name.c_str() || *end != '\0') continue;
        if (processAlive(static_cast<pid_t>(pid))) continue;
        std::filesystem::remove_all(entry.path(), ec);
        Poco::Logger::get("InstancePaths").information(
            "Removed stale instance directory of dead pid %ld", pid);
    }
}

} // namespace InstancePaths
} // namespace Core
} // namespace MediaDedup
