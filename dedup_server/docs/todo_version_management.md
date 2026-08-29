# VER-1 — git-derived version management, sidebar version display, build-pair handshake check

Status: designed 2026-08-28. Not yet implemented.

This document is the full specification. It is written so that an implementer
who has never seen this repo can follow it file by file. Where a decision looks
odd, the reason is stated inline — do not "simplify" past a stated reason. If
something here contradicts what you find in the code, stop and ask; do not
guess.

---

## 1. Goal and user-visible behavior

Three things, one feature:

1. **A version scheme with nothing to remember.** The version is *computed from
   git history* at build time — no VERSION file to bump, no tags to remember,
   no CI. Merging a PR to `main` bumps the minor automatically, because the
   version is (in part) a count of merge commits. Only the major is set by a
   human, in a tiny committed rules file.

2. **A version line in the sidebar.** Shown only when the advanced overlay is
   enabled (`@AppStorage("advancedOverlayEnabled")`), rendered directly above
   the progress bar in the navigation pane's status area, in the advanced
   overlay font (`Menlo` 9) and theme colors.
   - Built from `main`: shows the version, e.g. `v1.4.2 8488a3de`.
   - Built from any other branch: shows the **branch name** instead of the
     version, e.g. `code_managment 3fa9c11b`.
   - Built with uncommitted changes: an asterisk suffix on the first token,
     e.g. `v1.4.2* 8488a3de` or `code_managment* 3fa9c11b`.

3. **A build-pair check at startup.** The client and server each embed, at
   build time, the git commit they were built from. The existing pairing
   `Handshake` RPC is extended to exchange these. If they differ, **both
   sides log a clear error, the client shows it to the user, and both
   processes exit.** ("Built at the same time" is implemented as "built from
   the same commit" — two builds hours apart from the same commit still pass,
   which is the actual intent; a wall-clock comparison would break every time
   only one side is rebuilt.)

Decisions locked with the project owner on 2026-08-28:
- Version source: **derived from git** (option A below), not tags, not a
  VERSION file.
- Match rule: **version string AND git commit** must both be equal. Dirty
  flags are ignored for matching. Branch is informative only.
- On mismatch: **both processes exit** (owner explicitly chose this over
  server-stays-up; see §6.4 for the safety detail this forces).
- Dirty builds: **asterisk suffix** on the display; no effect on matching.

---

## 2. The version scheme

### 2.1 The rules file

New committed file at the **repo root**: `version.rules`

```
# VER-1 version rules. Only `major` is human-managed.
# minor  = number of merge commits on the current history since major_base
# patch  = number of non-merge commits since the most recent merge commit
major = 1
major_base = <FULL-SHA>
```

At implementation time, set `major_base` to the current `main` HEAD
(`git rev-parse main`), full 40-char SHA. (The ideal base — the merge commit
that will land VER-1 itself — doesn't exist yet when the file is written;
main's current HEAD is the closest stable anchor.) The scheme then starts at
`1.0.N`.

Parsing rules (the generator script, §3, is the only consumer): lines are
`key = value`, `#` starts a comment, whitespace around `=` is optional and
must be tolerated. Unknown keys are ignored (forward compatibility). If the
file is missing or `major`/`major_base` are absent or malformed, the script
must **fail loudly** (non-zero exit, message to stderr) — a silently wrong
version defeats the whole feature.

### 2.2 Version computation

All commands run at the repo root, against the checkout being built:

```
branch = git rev-parse --abbrev-ref HEAD          # e.g. "main", "code_managment"
commit = git rev-parse --short=8 HEAD             # e.g. "8488a3de"
dirty  = [ -n "$(git status --porcelain)" ]       # true if any tracked change
                                                  # (gitignored files do NOT count —
                                                  #  porcelain excludes them by default,
                                                  #  which matters: our own generated
                                                  #  files are gitignored, §3.3)

minor  = git rev-list --count --merges ${major_base}..HEAD
patch  = last_merge=$(git rev-list --merges -n 1 HEAD)
         if last_merge is empty: base=${major_base} else base=${last_merge}
         git rev-list --count ${base}..HEAD

version = "${major}.${minor}.${patch}"
```

Why this works: every PR merge to `main` creates one merge commit, so `minor`
increments by exactly one per merged branch, with no script run and no state
change. `patch` counts direct-to-main commits since the last merge and resets
to 0 at every merge. On a feature branch the numbers drift from main's — that
is fine and intentional, because branch builds *display the branch name, not
the version* (§7); the version is only authoritative on `main`.

Detached HEAD (`branch` = `HEAD`): keep the literal string `HEAD` as the
branch name. Not built from git at all (no `.git`, e.g. a source tarball):
the script emits `version = 0.0.0`, `branch = nogit`, `commit = unknown`,
`dirty = false`. Two such builds pair successfully with each other (equal is
equal); a nogit build never pairs with a git build. Do not special-case
further.

### 2.3 Bumping the major (the one manual procedure)

1. Edit `version.rules`: set `major = 2` and `major_base = $(git rev-parse main)`.
2. Commit and merge like any other change.

After the merge, minor/patch restart near zero because the counting base
moved. Document nothing else; there is nothing else.

---

## 3. The generator script

New file: `scripts/gen_version_info.sh` (mode 755). Single source of truth —
it computes the values **once** and writes **both** language embeddings, so
the server and client can never disagree by construction drift.

### 3.1 Contract

```
usage: gen_version_info.sh <repo-root>
writes: <repo-root>/dedup_server/generated/version_info.hpp
        <repo-root>/swift_ui/Sources/swift_ui/Generated/VersionInfo.swift
exit 0 on success; non-zero with a stderr message on any failure
```

`#!/bin/bash`, `set -euo pipefail`. Everything derived per §2.2, reading
`<repo-root>/version.rules`.

**Change detection is mandatory:** write each output to a temp file first and
only move it over the real file if `cmp -s` says the content differs.
Reason: CMake re-runs this script on *every* build (§4); if the file's mtime
changed every time, every build would recompile the files that include it.
With change detection, a rebuild with no git change is a no-op.

### 3.2 Output formats

`dedup_server/generated/version_info.hpp`:

```cpp
// GENERATED by scripts/gen_version_info.sh — do not edit, do not commit.
#pragma once
namespace MediaDedup { namespace VersionInfo {
inline constexpr const char *kVersion = "1.4.2";
inline constexpr const char *kBranch  = "main";
inline constexpr const char *kCommit  = "8488a3de";
inline constexpr bool        kDirty   = false;
// Ready-made display string per §7: version on main, branch elsewhere,
// asterisk when dirty, then the short commit.
inline constexpr const char *kDisplay = "v1.4.2 8488a3de";
}} // namespace MediaDedup::VersionInfo
```

`swift_ui/Sources/swift_ui/Generated/VersionInfo.swift`:

```swift
// GENERATED by scripts/gen_version_info.sh — do not edit, do not commit.
enum VersionInfo {
    static let version = "1.4.2"
    static let branch  = "main"
    static let commit  = "8488a3de"
    static let dirty   = false
    static let display = "v1.4.2 8488a3de"
}
```

`display` composition (compute once in the script, embed verbatim in both):

```
first  = (branch == "main") ? "v" + version : branch
if dirty: first += "*"
display = first + " " + commit
```

### 3.3 gitignore — this is load-bearing

Add to the repo-root `.gitignore`:

```
# VER-1: build-time generated version embeddings. MUST stay untracked —
# a tracked copy would dirty the tree on every build, which would flip the
# dirty flag inside the very file being generated (feedback loop).
dedup_server/generated/version_info.hpp
swift_ui/Sources/swift_ui/Generated/VersionInfo.swift
```

Note the contrast: the other files in `Generated/` (`dedup.pb.swift`,
`dedup.grpc.swift`) are **tracked**. Only `VersionInfo.swift` is ignored. Do
not gitignore the whole directory.

---

## 4. Server embedding (CMake)

In `dedup_server/CMakeLists.txt`, before the server/test targets are defined:

```cmake
# VER-1: regenerate version_info.hpp (and the Swift twin) on every build.
# The script self-no-ops when nothing changed, so this does not trigger
# rebuild cascades. ${CMAKE_SOURCE_DIR} here is dedup_server/, hence /..
add_custom_target(gen_version_info ALL
    COMMAND ${CMAKE_SOURCE_DIR}/../scripts/gen_version_info.sh ${CMAKE_SOURCE_DIR}/..
    BYPRODUCTS ${CMAKE_SOURCE_DIR}/generated/version_info.hpp
    COMMENT "VER-1: computing build version from git")
add_dependencies(media_dedup_server gen_version_info)
add_dependencies(all_unit_tests gen_version_info)   # tests include the header too
```

(Adjust the target names to whatever the real executable/test targets are
called in that file — verify, don't assume. `dedup_server/generated/` may need
`include_directories` or a `target_include_directories` addition so
`#include "generated/version_info.hpp"` resolves; follow how the file's other
include roots are set up.)

Also run the script **once during configure** (`execute_process`) so a fresh
`cmake ..` produces the header before the first compile scans includes.

Server usage:
- `server_server_initializer.cpp`: log one line at startup, e.g.
  `Vurio dedup server <kDisplay> starting` — this is the server-side half of
  "log a relevant message".
- `security_service.hpp`: the handshake check, §6.

## 5. Client embedding (SwiftPM)

SwiftPM has no reliable pre-build shell hook (plugin sandboxing blocks `git`),
so the Swift file is produced by the same script, invoked from the build
entry points people actually use:

1. Building the server regenerates it (§4 — the CMake target writes both
   files). In practice the server is rebuilt constantly, so the file is
   almost always fresh.
2. For Swift-only rebuilds, prepend the script to the documented build
   command. Update any repo scripts that run `swift build`
   (`scripts/run_app.sh`, `scripts/restart.sh` — check what they contain
   first) to run `scripts/gen_version_info.sh "$(git rev-parse --show-toplevel)"`
   beforehand.
3. Fresh clone: `swift build` before ever generating the file would fail to
   compile (`VersionInfo` unresolved). That is acceptable and self-explaining
   — but soften it by having the script create the file on configure (§4),
   and mention the script in the build section of the README if one exists.

**Staleness is not silent**: if someone builds the Swift app with a stale
generated file, the embedded commit won't match the server's and the §6 check
fires with a message that names both commits. The check is the backstop for
every gap in this section.

---

## 6. The build-pair handshake check

### 6.1 Proto changes (`dedup_server/src/api/dedup.proto`)

Extend the two existing messages — **add fields, never renumber existing
ones**:

```proto
message HandshakeRequest {
  string cookie = 1;
  // VER-1: client build identity, checked by the server.
  string client_version = 2;   // "1.4.2" (or "0.0.0" for nogit builds)
  string client_commit  = 3;   // short sha, "8488a3de"
  string client_branch  = 4;   // informative only, for logs
  bool   client_dirty   = 5;   // informative only, for logs
}

message HandshakeResponse {
  bool ok = 1;
  string session_token = 2;
  string error_message = 3;
  // VER-1: server build identity, always filled (success and failure),
  // so the client can display exactly what it is talking to.
  string server_version = 4;
  string server_commit  = 5;
  string server_branch  = 6;
  bool   server_dirty   = 7;
}
```

Regenerate the C++ stubs (the CMake protoc step does it) and the Swift stubs
via `scripts/gen_swift_protos.sh`. The regenerated `dedup.pb.swift` /
`dedup.grpc.swift` are tracked files — commit them.

### 6.2 The match rule (one function, unit-testable)

New small header `dedup_server/include/core/grpc_server/version_check.hpp`:

```cpp
#pragma once
#include <string>

namespace MediaDedup { namespace Core {

// VER-1 §6.2: the pairing rule. Version AND commit must match exactly.
// Dirty flags and branch names are deliberately NOT part of the rule
// (owner decision 2026-08-28): two dirty builds from the same commit pass.
// Empty client fields mean a pre-VER-1 client — mismatch by definition.
inline bool buildPairMatches(const std::string &client_version,
                             const std::string &client_commit,
                             const std::string &server_version,
                             const std::string &server_commit)
{
    if (client_version.empty() || client_commit.empty())
        return false;
    return client_version == server_version && client_commit == server_commit;
}

}} // namespace MediaDedup::Core
```

### 6.3 Server side (`security_service.hpp`)

`SecurityServiceImpl::Handshake` currently checks only the pairing cookie.
New order of checks:

1. Cookie check exactly as today. Wrong cookie → the existing rejection path,
   **no version disclosure** (don't leak build info to something that can't
   even present the cookie).
2. Cookie OK → fill `server_version/commit/branch/dirty` on the reply from
   `VersionInfo::k*` (always, from here on).
3. `buildPairMatches(...)` with the request fields:
   - **Match** → `ok=true`, session token, success log line as today but
     including both identities.
   - **Mismatch** → `ok=false`, no token, and:

```cpp
reply->set_error_message(
    "Version mismatch: client " + describe(client...) +
    " vs server " + std::string(VersionInfo::kDisplay) +
    ". Rebuild both from the same commit.");
Poco::Logger::get("GrpcSecurity").error(...same text...);
on_version_mismatch_();   // see §6.4
```

where `describe()` renders `<version-or-branch><*> <commit>` from the request
fields with the same rules as §3.2's display string.

### 6.4 Both sides exit — and why the exit must be injectable

Owner chose "both exit" knowing the trade-off (any stale client binary can
take the server down). Two implementation constraints follow:

1. **The reply must reach the client before the server dies**, or the client
   shows a generic transport error instead of the real message. So the server
   must NOT exit inline in the handler. Give `SecurityServiceImpl` a member
   `std::function<void()> on_version_mismatch_` whose **default** launches:

```cpp
std::thread([] {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    Poco::Logger::get("GrpcSecurity").error(
        "Exiting: client/server build mismatch (VER-1)");
    Poco::Util::ServerApplication::terminate();  // graceful Poco shutdown path
}).detach();
```

   `terminate()` releases `waitForTerminationRequest()`, so the normal
   shutdown sequence (`server_server_shutdown.cpp`) runs — scheduler stopped,
   DB closed. Do not call `std::exit`.

2. **Unit tests would die with the process** if the default ran in tests. The
   constructor must accept an override:
   `explicit SecurityServiceImpl(std::function<void()> on_mismatch = defaultExit);`
   Tests inject a counter lambda (§9). Check how the service is constructed
   in `grpc_server_core.cpp` and keep that call site using the default.

### 6.5 Client side (`swift_ui/Sources/swift_ui/GrpcClient.swift`, `pair()`)

Current behavior: 60 retries at 500 ms; on `resp.ok == false` it logs and
retries. Changes:

1. Fill the request:

```swift
let req = Vurio_Dedup_HandshakeRequest.with {
    $0.cookie = cookie
    $0.clientVersion = VersionInfo.version
    $0.clientCommit  = VersionInfo.commit
    $0.clientBranch  = VersionInfo.branch
    $0.clientDirty   = VersionInfo.dirty
}
```

2. On `resp.ok == false` where `resp.errorMessage` contains a version
   mismatch, or on `resp.ok == true` with
   `resp.serverCommit != VersionInfo.commit || resp.serverVersion != VersionInfo.version`
   (belt and braces: catches a server built before VER-1, whose fields come
   back empty) — treat as **fatal, do not retry**: break out of the loop and
   invoke a fatal-mismatch path. Distinguish mismatch from a plain cookie
   rejection by adding a machine-readable marker: have the server prefix the
   mismatch `error_message` with `VERSION_MISMATCH: ` and match on that
   prefix — do not parse prose.
3. The fatal path must hop to the main actor, log the full message, show a
   blocking alert, then terminate:

```swift
await MainActor.run {
    let alert = NSAlert()
    alert.alertStyle = .critical
    alert.messageText = "Vurio version mismatch"
    alert.informativeText = message   // the server's error_message, or a
                                      // locally-composed one for the empty-fields case
    alert.addButton(withTitle: "Quit")
    alert.runModal()
    NSApplication.shared.terminate(nil)
}
```

4. Cookie-rejection behavior (retry loop) stays exactly as it is today.
5. On **success**, stash `resp.serverVersion` etc. somewhere reachable if
   convenient (not required by this spec; the UI in §7 displays the client's
   own embedded identity, which the handshake has just proven identical).

---

## 7. The sidebar version display

Location: `swift_ui/Sources/swift_ui/VurioApp.swift`, `struct StatusBarView`
(≈ line 798 at time of writing). Its body is
`VStack(spacing: 0) { Divider(); VStack(spacing: 6) { [transient message] ;
HStack { progress bar ; status dot } } }`. The status bar now lives at the
bottom of the navigation pane (sidebar), attached via
`.safeAreaInset(edge: .bottom)`.

Add, as the **first row inside the inner `VStack(spacing: 6)`** — i.e. above
the transient message and above the progress bar row:

```swift
// VER-1: build identity, advanced-overlay only. Shows the version when
// built from main, the branch name otherwise; '*' marks a dirty build.
if advancedOverlayEnabled {
    Text(VersionInfo.display)
        .font(.custom("Menlo", size: 9))          // the advanced-overlay font
        .foregroundColor(Theme.textSecondary)      // quiet gray, theme scheme
        .frame(maxWidth: .infinity, alignment: .leading)
        .help("Build: \(VersionInfo.branch) @ \(VersionInfo.commit)\(VersionInfo.dirty ? " (uncommitted changes)" : "")")
}
```

`StatusBarView` does not currently read the overlay flag — add
`@AppStorage("advancedOverlayEnabled") private var advancedOverlayEnabled = false`
to it (same declaration as the existing one at ≈ line 608).

Do NOT put the version inside the hover callout/stats panel — it is a fixed
line in the bar itself, per the requirement "above the progress bar". Keep
`Text`, not a button; no interaction beyond the tooltip.

---

## 8. Edge cases (decided — implement as stated)

| Case | Behavior |
| --- | --- |
| Dirty tree at build | `*` suffix on display; ignored by the match rule. |
| Client and server both dirty, same commit | Pair succeeds. |
| Only Swift app rebuilt after a new commit | Commit mismatch → both exit with the explanatory message. This is intended: "built together" is the contract. |
| Pre-VER-1 client hits new server | Empty version fields → mismatch → both exit. One-time migration cost, accepted. |
| New client hits pre-VER-1 server | Response fields empty → client-side belt-and-braces check (§6.5.2) fires → client exits with message. Old server keeps running (it has no check). |
| Wrong cookie AND wrong version | Cookie rejection wins; no version info disclosed; no exit (today's retry behavior). |
| No `.git` (tarball build) | `0.0.0 / nogit / unknown`; pairs only with an identical build. |
| Detached HEAD | Branch displays as `HEAD`; everything else normal. |
| `version.rules` missing/malformed | Generator fails the build loudly. Never default. |

---

## 9. Tests

Server (`dedup_server/tests/unit/`, follow the style of existing tests):

1. **`version_check` table test** — `buildPairMatches`: equal → true; commit
   differs → false; version differs → false; both empty client fields →
   false; matching but both-dirty inputs are not part of the function
   signature (dirty is excluded by design — the test documents that by its
   absence).
2. **Handshake mismatch test** — construct `SecurityServiceImpl` with an
   injected `on_mismatch` counter (§6.4.2). Valid cookie + wrong commit →
   `ok == false`, `error_message` starts with `VERSION_MISMATCH: `, counter
   fired once, `server_commit` filled in the reply. Valid cookie + matching
   fields → `ok == true`, counter never fired.
3. **Wrong-cookie test still green** — and assert the reply carries **no**
   server version fields on cookie failure (§6.3.1).

Generator script (manual acceptance, no harness needed): run
`scripts/gen_version_info.sh .` twice; second run must not touch file mtimes
(`stat` before/after). Touch a tracked file, rerun, confirm `dirty = true`
and `*` in `display`. Revert, rerun, confirm it clears.

Swift: build-level verification only (the UI line and the alert are
manual-pass items below).

Acceptance checklist (manual, on the live app):
- [ ] Advanced overlay ON → version line appears above the progress bar in
      the sidebar, Menlo 9, gray; OFF → absent, no layout shift beyond the
      line itself.
- [ ] Build both from `main` → line reads `vX.Y.Z <commit>`; from a branch →
      branch name instead.
- [ ] Merge any PR to main, rebuild → minor incremented by exactly 1, patch
      reset to 0.
- [ ] Rebuild only the client after a new commit → alert names both builds,
      client exits after "Quit", server log shows the mismatch and the server
      process exits within ~2 s.
- [ ] Matching builds → app works exactly as before.

---

## 10. Touch list (expected files)

| File | Change |
| --- | --- |
| `version.rules` | new, committed (§2.1) |
| `scripts/gen_version_info.sh` | new, committed (§3) |
| `.gitignore` | ignore the two generated files (§3.3) |
| `dedup_server/CMakeLists.txt` | custom target + configure-time run (§4) |
| `dedup_server/generated/version_info.hpp` | generated, never committed |
| `swift_ui/Sources/swift_ui/Generated/VersionInfo.swift` | generated, never committed |
| `dedup_server/src/api/dedup.proto` | Handshake fields 2–5 / 4–7 (§6.1) |
| `swift_ui/Sources/swift_ui/Generated/dedup.pb.swift`, `dedup.grpc.swift` | regenerated, committed |
| `dedup_server/include/core/grpc_server/version_check.hpp` | new (§6.2) |
| `dedup_server/include/core/grpc_server/security_service.hpp` | version check + injectable exit (§6.3–6.4) |
| `dedup_server/src/core/grpc_server/grpc_server_core.cpp` | only if the service construction needs the default hook spelled out (§6.4) |
| `dedup_server/src/core/server_server_initializer.cpp` | startup log line with `kDisplay` (§4) |
| `swift_ui/Sources/swift_ui/GrpcClient.swift` | request fields, fatal path, alert+exit (§6.5) |
| `swift_ui/Sources/swift_ui/VurioApp.swift` | `StatusBarView` version line (§7) |
| `dedup_server/tests/unit/test_version_check.cpp` (or fold into an existing security test file) | §9 |
| `scripts/run_app.sh`, `scripts/restart.sh` | prepend generator call if they run `swift build` (§5.2) |

Implementation order that keeps every intermediate commit green:
**VER-1a** rules file + generator + gitignore + CMake wiring + startup log →
**VER-1b** proto fields + stub regen (both sides build, nothing enforced) →
**VER-1c** server check + injectable exit + unit tests →
**VER-1d** client request fields + fatal path + alert →
**VER-1e** sidebar display line + manual acceptance pass.
