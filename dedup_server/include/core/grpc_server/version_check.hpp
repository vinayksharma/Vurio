#pragma once

// VER-1 §6.2: the build-pair rule, factored out so it is unit-testable
// without constructing a gRPC service.

#include <string>

namespace MediaDedup {
namespace Core {

// Version AND commit must match exactly. Dirty flags and branch names are
// deliberately NOT part of the rule (owner decision 2026-08-28): two dirty
// builds from the same commit pass. Empty client fields mean a pre-VER-1
// client — mismatch by definition.
inline bool buildPairMatches(const std::string &client_version,
                             const std::string &client_commit,
                             const std::string &server_version,
                             const std::string &server_commit)
{
    if (client_version.empty() || client_commit.empty())
        return false;
    return client_version == server_version && client_commit == server_commit;
}

} // namespace Core
} // namespace MediaDedup
