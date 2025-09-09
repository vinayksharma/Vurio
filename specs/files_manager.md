### FilesManager Technical Specification

Pre-req: Adhere to Project Operating Rules in `specs/CURSOR_RULES.md`. If this spec and cursor_rules.md ever conflict, cursor_rules.md wins.

## 1) Goal & Scope

- Implement a scheduler-driven FilesManager that periodically scans registered media locations and synchronizes the `scanned_files` table.
- Integrate with existing components: `FilesService` (media locations), `FileScanner` (enumeration), `ScannedFilesOps` (DB ops), `UnifiedObservableConfigManager` (config), and `ThreadPoolManager` (TPM) for scheduling capacity and graceful shutdown.
- Non-goals: content hashing, MIME sniffing, EXIF, duplicate detection (handled later by processing pipelines).

## 2) Placement & Build

- Location: `src/orchestration/files_manager.cpp`, header under `include/orchestration/files_manager.hpp`.
- Unit tests: `tests/unit/test_files_manager.cpp` (added to `all_unit_tests`).

## 3) Responsibilities

- Discover directories to scan from `FilesService::listMediaLocations()` (backed by `user_settings` with `mediaLocation:` keys).
- Load current `scanned_files` rows into an in-memory map keyed by absolute `file_path` for O(1) lookup during scans.
- Traverse each directory via `MediaDedup::Files::scan()` using current scanner options.
- For each emitted `FileRecord`:
  - On per-entry error: log at `warn` and persist brief error fields; continue. Root-level errors: log `warn` only (no persistence).
  - On success: upsert into `scanned_files` with metadata. If changed, reset processing flags to 0.
- Ensure only one scan run is active at a time; skip scheduling if a run is still in progress.
- Shutdown cleanly on Ctrl+C/exit (drain via TPM with configured kill timeout).

## 4) Public API (FilesManager)

```
namespace MediaDedup::Orchestration {
  class FilesManager {
  public:
    FilesManager(std::shared_ptr<UnifiedObservableConfigManager> config,
                 std::shared_ptr<DatabaseManager> db,
                 std::shared_ptr<ThreadPoolManager> tpm,
                 std::shared_ptr<FilesService> filesService);

    void initialize();                  // subscribe to config, ensure tables, prepare internal state
    // Performs a full pass across all locations; SchedulerService will call this on interval
    void runOnce();
    // Optional immediate trigger (returns once the run is queued or skipped if active)
    void triggerScanNow();

  private:
    // internal helpers (no scheduling logic here)
  };
}
```

## 5) Configuration (observable)

- New keys (observable; defaults shown):
  - `files.manager.scan.intervalMs` (int, default: 300000) – interval between full scans in milliseconds.
  - `files.manager.enabled` (bool, default: true) – allow disabling scheduling without rebuilding.
  - `tpm.types.fileScan.share` (int; allowed values: 0 or 1; default: 1) – per-type share for the FilesManager task. 0 disables dispatch; 1 enables with full share (no fractional shares for this type).
  - SchedulerService (global defaults; per-job overrides optional under `scheduler.jobs.<jobId>.*`):
    - `scheduler.jitter.enabled` (bool, default: false)
    - `scheduler.jitter.percent` (int 0..100, default: 0)
    - `scheduler.backoff.enabled` (bool, default: true)
    - `scheduler.backoff.initialMs` (int, default: 1000)
    - `scheduler.backoff.maxMs` (int, default: 30000)
    - `scheduler.backoff.multiplier` (double, default: 2.0)
    - `scheduler.backoff.jitterPercent` (int 0..100, default: 10)
    - `scheduler.drift.mode` (string: `anchored` | `fixedDelay`; default: `anchored`)
    - `scheduler.drift.maxDriftMs` (int, default: 60000)
    - Per-job overrides (all optional):
      - `scheduler.jobs.<jobId>.jitter.enabled`
      - `scheduler.jobs.<jobId>.jitter.percent`
      - `scheduler.jobs.<jobId>.backoff.enabled`
      - `scheduler.jobs.<jobId>.backoff.initialMs`
      - `scheduler.jobs.<jobId>.backoff.maxMs`
      - `scheduler.jobs.<jobId>.backoff.multiplier`
      - `scheduler.jobs.<jobId>.backoff.jitterPercent`
      - `scheduler.jobs.<jobId>.drift.mode`
      - `scheduler.jobs.<jobId>.drift.maxDriftMs`
- Uses existing `FileScanner` keys (already implemented):
  - `filesservice.scan.recursive` (bool, default: true)
  - `filesservice.scan.followSymlinks` (bool, default: false)
  - `filesservice.scan.includeHidden` (bool, default: true)
  - `filesservice.log.level` (string: trace|debug|info|warn|error; default: trace)
- Behavior on config changes:
  - Interval or enabled toggles are applied for subsequent scheduling cycles.
  - Scanner options are read at the start of each run.
  - SchedulerService observes its config keys. Changes to jitter/backoff/drift and per-job overrides take effect on subsequent schedules for that job.

## 6) Data Model Mapping (FileRecord -> scanned_files)

- For each file:
  - `file_path` = absolute path from `FileRecord.fullPath` (native separators)
  - `relative_path` = path relative to the scanned root (when determinable)
  - `share_name` = best-effort network share or volume name (if available)
  - `file_name` = basename
  - `file_metadata` (JSON):
    ```json
    {
      "sizeBytes": <uint64>,
      "modifiedAtEpochMs": <int64>,
      "deviceId": "...",
      "inode": "...",
      "symlinkTarget": "...",
      "isHidden": true|false
    }
    ```
  - `is_network_file` = best-effort flag based on platform detection (optional; default false until implemented)
  - `last_seen_at` = TIMESTAMP updated on each successful scan emit (CURRENT_TIMESTAMP on insert/update)
  - Brief error persistence fields:
    - `last_error_code` TEXT NULL (e.g., DIR_NOT_FOUND, PERMISSION_DENIED)
    - `last_error_message` TEXT NULL (brief message)
    - `last_error_errno` INTEGER NULL
    - `last_error_at` TIMESTAMP NULL
    - On a successful emit for the same `file_path` in a later run, clear `last_error_*` and update `last_seen_at`.
- Processing flags:
  - On insert: `processed_fast = processed_balanced = processed_quality = 0` (unprocessed)
  - On detected change: reset same flags to `0` to signal reprocessing is needed.

## 7) Change Detection

- In-memory lookup by `file_path` (unordered_map).
- Compare new `FileRecord` against previously persisted metadata:
  - If identity (deviceId/inode) differs → treat as changed.
  - Else if size or modified time differs → treat as changed.
  - Else if attributes-only differ → optional (currently treated as unchanged for processing).
- On changed: upsert row and reset processed flags to `0`.
- On new: insert row with processed flags set to `0`.

## 8) Scheduling & Concurrency

- FilesManager maintains an atomic `isRunning_` guard to prevent overlapping runs.
- Scheduling is delegated to a reusable `SchedulerService` under `orchestration` with a generic registration API:
  - `register(jobId, intervalMs, typeKey, callback)` (supports dynamic interval updates via config subscription per job)
  - `unregister(jobId)`
  - Internally uses `ThreadPoolManager` to dispatch via `typeKey`
- FilesManager auto-registers on server startup:
  - `jobId = "fileScan"`, `intervalMs = files.manager.scan.intervalMs`, `typeKey = "fileScan"`, `callback = FilesManager::runOnce`
- `tpm.types.fileScan.share` is constrained to 0 or 1 for this task.
- If a run is still active when the next interval elapses, `SchedulerService` skips scheduling (no queue buildup).
- On shutdown, `SchedulerService` stops triggering and allows the in-flight run to finish within `tpm.killTimeoutMs`.

Scheduling semantics:

- Jitter: When enabled, each schedule computes a random offset within ±`jitter.percent` of the interval (global or job override). Jitter is applied per run to avoid synchronized starts.
- Backoff: On callback failure (exception thrown), the next attempt uses exponential backoff based on `initialMs`, `multiplier`, and capped at `maxMs`, with optional backoff jitter. On success, backoff state resets for that job.
- Drift control: In `anchored` mode, the cadence is anchored to the nominal schedule (e.g., every N ms), and actual start times are adjusted to not exceed `drift.maxDriftMs` from the anchor. In `fixedDelay` mode, the next run is scheduled relative to the completion time. Configurable per job via overrides; defaults are global.

## 9) Thread Safety

- In-memory index guarded by `std::mutex` or `std::shared_mutex` (read-heavy pattern).
- All DB operations obtain sessions via `DatabaseManager::acquireSessionLease()` (RAII, pooled, timeout/backoff per config).
- `FilesService` and `UnifiedObservableConfigManager` accessed through shared pointers; callbacks unregistered on stop.

## 10) Logging

- Use project logging levels (trace, debug, info, warn, error):
  - trace: run start/stop, directory enter/leave, per-entry actions, counters
  - info: run summary (directories visited, files inserted/updated)
  - warn: access errors from `FileScanner` records, skipped schedules
  - error: unexpected exceptions; safe to continue next cycle

## 11) Error Handling

- Per-entry access errors from `FileScanner` are logged and do not abort the run.
- Root-level errors for a media location (e.g., DIR_NOT_FOUND) are logged once per run at warn level (no persistence).
- Database errors: log error and continue to next file/location; run completes with partial updates.
- Error persistence: When `FileScanner` emits an error for a path under a scanned root, upsert or create a stub row keyed by `file_path` (if permissible) with `last_error_*` populated. Clear on next success for the same `file_path`.

## 12) Testing

- Unit tests (`test_files_manager.cpp`):
  - Inserts: new files in temp directories are inserted with flags reset to 0.
  - Updates: touching size/mtime resets flags to 0 and updates metadata.
  - Skip overlap: a second trigger during an active run is skipped.
  - Respects `filesservice.scan.recursive` when toggled via config.
  - Interval changes take effect on subsequent schedules.
  - Error persistence: per-entry errors populate `last_error_*`; a subsequent successful emit clears them.
  - SchedulerService: interval-driven triggering, skip when active, honors `tpm.types.fileScan.share` (0/1).
- Integration test (optional future): end-to-end with TPM and temporary config, asserting periodic runs and DB state.

## 13) Acceptance Criteria

- SchedulerService triggers `FilesManager::runOnce()` based on `files.manager.scan.intervalMs` while enabled.
- Only one run may be active at a time; subsequent schedules are skipped if busy.
- New files are inserted; changed files update metadata and reset processed flags to 0.
- Honors observable scanner options and interval changes without restart.
- Shuts down cleanly with TPM draining on Ctrl+C/exit.

## 14) Implementation Notes

- Relative path derivation is best-effort: when scanning directory `D`, compute `relative_path` as `path.lexically_relative(D)` if possible.
- `file_metadata` JSON is versionable; include a `schemaVersion` key if future evolution is likely.
- Network share detection can be stubbed initially on macOS/Linux and expanded later.

## 15) Follow-ups (out of immediate scope)

- Deletion tracking (identify and mark deleted files efficiently) – revisit with perf profiling and possibly batched diffs.
- Expose FilesManager status over Web API (e.g., last run time, counts).
- Backoff strategy for repeated root failures.

---

### Remaining Questions

- Scheduler scope: Should `Scheduler` be generic and reusable for other periodic tasks (e.g., future pipelines), or dedicated to `FilesManager` only?
