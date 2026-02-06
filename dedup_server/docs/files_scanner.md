File Scanner – C++-Only Implementation Spec (updated for cursor_rules.md)

Pre-req: Adhere to Project Operating Rules in cursor_rules.md. If this spec and cursor_rules.md ever conflict, cursor_rules.md wins.

⸻

0. Workflow, Commits & Scripts (from cursor_rules.md)
   • Workflow (Plan → Confirm → Implement):
   1. Draft a concise implementation plan (scope, files to touch, tests to add).
   2. Pause for user confirmation before coding.
   3. Implement in small, incremental steps, building & testing after each logical change.
      • Commit/Push: Do not commit or push unless the user explicitly asks.
      • Tests: Put all tests under /tests. Add unit tests to all_unit_tests binary (invoked by rebuild). Keep all_tests updated to run all test types.
      • Failing tests: If new failures appear, prefer fixing implementation; only modify tests as a last resort and explicitly inform the user.
      • Scripts: Put helper/one-offs under /scripts (except root build.sh, rebuild, start).

Checklist: After each task/subtask, re-read the Rules Checklist in cursor_rules.bd and fix gaps immediately.

⸻

1. Scope & Goals

Implement a C++17+ File Scanner that:
• Resides in src/filesservice/.
• Scans a specified directory, recursively by default.
• Emits a C++ struct for every file encountered and for any access error.
• Main scanning function returns no value (events carry all information).
• Exposes a comparison API to determine if a file has been modified between two emitted structs.
• Logs telemetry to console at trace level (other levels available: error, warn, info, debug).
• Subscribes to configuration changes for any used property and reacts at runtime.

Non-goals: content hashing, MIME sniffing, EXIF, persistence.

⸻

2. Project Layout

src/
filesservice/
FileScanner.h
FileScanner.cpp
FileScannerOptions.h
FileRecord.h
FileScannerErrors.h
ConfigKeys.h # config key names used by FileScanner
PlatformPaths.h
PlatformPaths.cpp
internal/
Stat.h
Stat.cpp
ShareDetection.h
ShareDetection.cpp
Logger.h # console logger (trace/debug/info/warn/error)
Logger.cpp
tests/
filesservice/
FileScannerTests.cpp
scripts/
(helper scripts if needed)

⸻

3. Public API

3.1 FileScannerOptions.h

#pragma once
namespace filesservice {

struct FileScannerOptions {
bool recursive = true; // default: true (may be overridden by config)
bool followSymlinks = false; // reserved; keep false unless needed
bool includeHidden = true; // cross-platform best-effort
};

} // namespace filesservice

3.2 FileScannerErrors.h

#pragma once
#include <string>

namespace filesservice {

enum class ErrorCode {
OK = 0,
DIR_NOT_FOUND,
PERMISSION_DENIED,
SHARE_OFFLINE,
STAT_FAILED,
SYMLINK_LOOP,
PATH_UNREADABLE,
UNKNOWN
};

const char\* to_string(ErrorCode c);

} // namespace filesservice

3.3 FileRecord.h

#pragma once
#include <cstdint>
#include <string>
#include <chrono>

#include "FileScannerErrors.h"

namespace filesservice {

using TimePoint = std::chrono::system_clock::time_point;

struct FileRecord {
// Identity & pathing
std::string fileName; // basename
std::string fullPath; // absolute, native separators
std::string extension; // lowercased ("" if none)

// Network share awareness
std::string networkSharePath; // UNC or mount root; empty if N/A
std::string networkShareDriveName; // e.g., "Z:" or volume name; empty if N/A
bool isShareMapped = false; // true if accessible at emit time

// Stats
TimePoint createdAt{}; // UTC
TimePoint modifiedAt{}; // UTC
std::uint64_t fileSizeBytes = 0;

// Low-level IDs
std::string deviceId; // volume serial or st_dev
std::string inode; // file index or st_ino

// Attributes
std::string symlinkTarget; // absolute target if symlink; else empty
bool isHidden = false;

// Error (for successes: error == OK)
ErrorCode error = ErrorCode::OK;
int platformErrno = 0;
std::string errorMessage;

bool hasError() const noexcept { return error != ErrorCode::OK; }
};

enum class ChangeKind {
Unchanged,
SizeOrTimeChanged,
IdentityChanged,
AttributesChanged
};

} // namespace filesservice

3.4 FileScanner.h

#pragma once
#include <functional>
#include <filesystem>
#include "FileRecord.h"
#include "FileScannerOptions.h"

namespace filesservice {

using FileCallback = std::function<void(const FileRecord&)>;

/\*\*

- Scan a directory and emit a FileRecord for each file or access error.
- Returns no value; results stream via callback.
-
- Root-unreadable behavior:
- - Emit one FileRecord with .fullPath=root and error set appropriately,
-     then return.
  \*/
  void scan(const std::filesystem::path& directory,
  const FileScannerOptions& options,
  const FileCallback& onFile);

/\*_ Overload with default options (recursive=true). _/
inline void scan(const std::filesystem::path& directory,
const FileCallback& onFile) {
scan(directory, FileScannerOptions{}, onFile);
}

/\*_ True if a "meaningful" change occurred (identity/size/time/attributes). _/
bool isModified(const FileRecord& previous, const FileRecord& current);

/\*_ Classification for finer control over change semantics. _/
ChangeKind classifyChange(const FileRecord& previous,
const FileRecord& current);

} // namespace filesservice

⸻

4. Behavior & Semantics 1. Traversal
   • recursive defaults to true (unless configuration overrides at runtime).
   • Non-recursive scans only enumerate top-level entries.
   • Per-entry success → emit FileRecord with error=OK.
   Per-entry failure → emit FileRecord with error!=OK, platformErrno, errorMessage. Continue. 2. Root errors
   • Missing/not a directory → emit one record with DIR_NOT_FOUND and return.
   • Root permission denied → emit one record with PERMISSION_DENIED and return. 3. Symlinks & loops
   • With followSymlinks=false, emit the symlink itself; set symlinkTarget if resolvable.
   • Maintain a visited set of (deviceId, inode) to avoid cycles. 4. Network shares
   • Windows: detect \\server\share and mapped drives (e.g., Z:). Set networkSharePath, networkShareDriveName, isShareMapped. If offline, set isShareMapped=false and use SHARE_OFFLINE where applicable.
   • macOS: detect /Volumes/<Name>.
   • Linux: detect CIFS/SMB/NFS via /proc/mounts or statfs. 5. Timestamps
   • Convert to UTC TimePoint. Prefer birth/creation time if available; otherwise documented fallback. 6. Hidden files
   • Windows hidden attribute; Unix prefix .. 7. No accumulation
   • Stream via onFile immediately; do not store the entire result set.

⸻

5. Change Detection
   • Identity change → (deviceId, inode) differs ⇒ IdentityChanged (treat as modified).
   • Fast metadata change → fileSizeBytes or modifiedAt differs ⇒ SizeOrTimeChanged.
   • Attribute-only change → share fields / isHidden / symlinkTarget differ, identity & fast metadata same ⇒ AttributesChanged.
   • Unchanged → none of the above.

inline bool isModified(const FileRecord& a, const FileRecord& b) {
return classifyChange(a, b) != ChangeKind::Unchanged;
}

⸻

6. Configuration Integration (required by cursor_rules.bd)
   • Define config keys in ConfigKeys.h (example names):
   • filesservice.scan.recursive (bool; default: true)
   • filesservice.scan.followSymlinks (bool; default: false)
   • filesservice.scan.includeHidden (bool; default: true)
   • filesservice.log.level (string: "trace"|"debug"|"info"|"warn"|"error"; default: "trace")
   • Subscribe to config changes at runtime:
   • When a key changes, the scanner must immediately update in-memory behavior for new scans.
   • Keep the default config generator in sync with new/changed keys.
   • Update config reference/docs whenever keys are added/changed.

Implementation detail: provide a small adapter (e.g., ConfigAdapter) that reads current values on scan() start, and registers change callbacks with the host config system (whatever the project uses).

⸻

7. Logging (console)
   • Implement a minimal console logger (internal/Logger.\*) with levels: trace, debug, info, warn, error.
   • Default level = trace (telemetry-heavy), can be overridden by filesservice.log.level.
   • Suggested events:
   • trace: scan start/stop, directory enter/leave, per-entry decisions, symlink handling, share mapping results, emission counts.
   • debug: mount table parsing, platform timing conversions.
   • info: root summary (dirs visited, files emitted).
   • warn: recoverable access errors (also emitted as error records).
   • error: root-level failure records (also emitted).

⸻

8. Implementation Notes
   • Use std::filesystem for traversal and absolute normalization; keep native separators in fullPath.
   • Platform IDs:
   • Windows: GetFileInformationByHandleEx → volume serial + file index.
   • POSIX: stat.st_dev / stat.st_ino.
   • Lowercase extension (strip leading dot).
   • Always emit something for every encountered entry (success or error), ensuring a complete audit trail.

⸻

9. Testing (per cursor_rules.bd unified policy)
   • Location: /tests/filesservice/FileScannerTests.cpp.
   • Add tests to all_unit_tests; keep all_tests running all types.
   • Minimum unit tests:
   1. Root missing → single DIR_NOT_FOUND record then return.
   2. Non-recursive vs recursive enumeration counts.
   3. Emits records for files w/ and w/o extensions.
   4. Hidden detection (Windows attribute & dotfiles).
   5. Symlink loop avoided; symlinkTarget set when resolvable.
   6. Permission-denied subtree emits error records and continues.
   7. Network share detection (platform-stubbed): mapped vs offline.
   8. Timestamp normalization sanity.
   9. Large tree streaming (callback increments; no bulk memory).
   10. classifyChange cases: Unchanged / SizeOrTimeChanged / IdentityChanged / AttributesChanged.
   11. Config change reactivity: changing filesservice.scan.recursive and filesservice.log.level between scans takes effect without restart.

After each logical test or feature, build & run tests before proceeding.

⸻

10. Acceptance Criteria
    • Implementation under src/filesservice/; helper scripts under /scripts (if any).
    • scan(path, options, onFile) compiles and returns void; default overload uses recursion.
    • For every file or access error, a FileRecord is emitted; errors live inside the struct.
    • isModified and classifyChange behave per §5.
    • Console logging at trace by default; other levels available.
    • Configuration: reads defaults, subscribes to runtime changes, updates behavior on next scan; default config generator & docs updated.
    • Tests located under /tests, integrated into all_unit_tests and all_tests.
    • Team followed Workflow & Rules Checklist from cursor_rules.bd.

⸻

11. Example

#include "filesservice/FileScanner.h"
#include <iostream>

using namespace filesservice;

int main() {
// Use defaults; respects current config values at call time.
scan("Z:/Projects", [](const FileRecord& rec) {
if (rec.hasError()) {
std::cerr << "[error] " << rec.fullPath << " -> "
<< to_string(rec.error) << " : " << rec.errorMessage
<< " (errno=" << rec.platformErrno << ")\n";
return;
}
std::cout << "[file] " << rec.fullPath << " (" << rec.fileSizeBytes << " bytes)\n";
});
}

⸻

End of spec (aligned with cursor_rules.bd).
