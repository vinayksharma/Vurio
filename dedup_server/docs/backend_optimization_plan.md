# Backend Optimization Plan

Actionable work plan produced from a five-dimension review (architecture, database,
concurrency, dedup algorithm, media pipeline) of `dedup_server/` on 2026-07-31.
Findings were verified by reading the code and, where noted, **measured against the
live database** (`data/dedup_server.db`, 5.6 GB, 160,319 scanned_files / 34,900
duplicate_groups / 140,331 image_artifacts) and the live caches
(`data/cache.thumbnails`, 59,326 files / 2.1 GB).

**How to use this file:** each task has an ID (e.g. `DB-1`), the files involved,
the problem, the change, and how to verify. Tasks are grouped into phases ordered
by leverage; tasks within a phase are mostly independent unless a "Depends on" is
noted. Line numbers are as of commit `d576c12` — re-locate by the quoted
identifiers if the files have drifted. Check tasks off as they land.

**Standing verification for every phase:** `cd dedup_server && ./build.sh` (or
`cmake --build build`), run unit tests in `tests/unit/`, then start the server
and exercise VU/BINNED/viewer from the SwiftUI app.

---

## Phase 0 — Quick wins (one small PR; do first)

### QW-1. Add `-strip` to thumbnail/preview generation ✅ 1 line

> **STATUS: ✅ DONE 2026-08-02 (validated).**
> Arg-building extracted into testable statics `ThumbnailGenerator::buildMagickArgs`
> and `ImageMagickTranscoder::buildPreviewMagickArgs`, both now emitting `-strip`
> right after the input. Tests: `tests/unit/test_magick_args.cpp` (MagickArgsTest, 6
> tests). Full suite green (246 tests). Measured on a live preview: 128px thumbnail
> 81,880 B → 4,140 B, 0 profiles. Existing bloated cache + `thumbnail_cache` rows
> purged post-validation; thumbnails regenerate small on demand.
- **Files:** `src/utils/thumbnail_generator.cpp:94-135` (`runMagick`, args at :102-116);
  same omission in `src/media_processors/image/backends/image_magick_transcoder.cpp:203-215`.
- **Problem (measured):** thumbnails carry full EXIF/XMP/ICC profiles. Worst sampled
  file: 128×72 thumbnail of 120,941 B where ~118 KB is metadata (97.7%). Live cache
  is 2.1 GB where ~220 MB would do. Bloat also keeps the cache permanently over its
  2048 MB limit, which makes eviction (MP-2) run on every single write.
- **Change:** add `-strip` to the arg vector (or replace `-resize` with `-thumbnail`,
  which implies strip). Keep `-auto-orient -colorspace sRGB`.
- **Verify:** regenerate one thumbnail; `magick identify -verbose` shows no
  `Profile-*` entries; file size drops ~10×.

### QW-2. Stop wiping the QHD preview cache on shutdown ✅ 1 line

> **STATUS: ✅ DONE 2026-08-02 (validated).**
> `MediaProcessor::shutdown()` no longer calls `disk_cache_->clearCache()` (comment
> left in place explaining why). Manual clearing via the ClearAllCaches RPC is
> unaffected (and clears files + DB rows together). Regression test:
> `MediaProcessorShutdownTest.ShutdownPreservesTranscodingDiskCache` in
> `tests/unit/test_media_processor.cpp` (fixture split into `SetUpCore()` so the
> test runs without the test-image assets).
- **File:** `src/media_processors/media_processor.cpp:107-113` (`MediaProcessor::shutdown()`).
- **Problem:** unconditionally calls `disk_cache_->clearCache()` on the shared
  transcoding cache (the persistent preview store injected at
  `src/core/server_server_initializer.cpp:326`). The `preview_cache` DB rows survive,
  so on next start every RAW fails the `std::filesystem::exists` check and is
  re-transcoded (~1.5 s each → hours per restart). Evidence: `data/cache.previews`
  holds 19 files against a 5120 MB limit.
- **Change:** delete the `clearCache()` call. There is no `cache.disk.clearOnStartup`
  property (only `cache.thumbnail.clearOnStartup` is declared at
  `src/config/config_manager_factory.cpp:472-475`), so persistence is the intent.
- **Verify:** restart server; previously generated previews are served without
  re-transcode (no `magick` spawns for already-cached RAWs).

### QW-3. Fix the null pause-callback capture ✅ 1 line move

> **STATUS: ✅ DONE 2026-08-02 (validated).**
> `setPauseCallback` moved below the `duplicateFinder` job registration in
> `server_server_initializer.cpp`, with an ORDERING IS LOAD-BEARING comment; the
> callback now captures a live `duplicate_finder_`. New tests:
> `SchedulerServiceTest.PauseInvokesPauseCallbackEachTime` and
> `PauseWithoutCallbackIsSafe` pin the callback contract the wiring relies on.
- **File:** `src/core/server_server_initializer.cpp:354` vs `:394`.
- **Problem:** `setPauseCallback([fm = files_manager_, df = duplicate_finder_](){ ... })`
  captures `duplicate_finder_` **by value while it is still nullptr** (it is
  constructed 40 lines later at :394). `abortCurrentFind()` is never called, so
  pausing the scheduler during a dedup run leaves the parent task blocked forever
  on `latch_cv.wait` (see CC-3), which then hangs the drain loops in
  `web_handlers_duplicates.cpp:1296-1300` (unbounded) and
  `web_handlers_cache.cpp:381-386` (15 s then 409).
- **Change:** move the `setPauseCallback` call below the `duplicate_finder_`
  construction.
- **Verify:** trigger integrity check (which pauses the scheduler) while a dedup
  run is active; it completes instead of hanging.

### QW-4. Constrain the spawned `magick` subprocesses ✅ ~5 lines

> **STATUS: ✅ DONE 2026-08-02 (validated).**
> Implemented as per-launch env (cleaner than global `setenv`, covers every
> spawn path): `ToolPaths::magickResourceEnv()` returns the MAGICK_* limits
> (THREAD=1, MEMORY=256MiB, MAP=512MiB, AREA=1GiB, DISK=2GiB, WIDTH/HEIGHT=65536);
> `runProcessWithTimeout` gained an `extra_env` param; all four magick spawn
> sites pass it (thumbnail_generator, image_magick_transcoder, and the two raw
> launches in image_processor.cpp — the EXIF full-RAW decodes). Tests:
> `MagickLimitsTest.EnvContainsAllResourceCaps` + `RealMagickHonorsLimits`
> (spawns the real binary and asserts the reported caps).
>
> **QW-4b (contingency, NOT implemented):** if real-world magick timeouts appear
> on legitimate huge files (fallback full-demosaic path only), implement
> reactive escalation: retry once with elevated limits (~1GiB) + longer timeout,
> serialized through a single large-decode slot. Prefer this over predictive
> per-file sizing (keeps the concurrency memory budget intact, no dimension
> heuristics). Do not build until the logs demonstrate the need.
- **File:** `src/media_processors/image/backends/image_magick_transcoder.cpp:84-94`
  (in-process `SetMagickResourceLimit` block, including "1 thread is critical" comment).
- **Problem (verified against `/opt/homebrew/bin/magick`):** all real decoding happens
  in subprocesses (`:203-229`, `src/utils/thumbnail_generator.cpp:118` via
  `src/utils/subprocess_runner.cpp`), which inherit none of the in-process limits.
  Each child runs up to **13 OpenMP threads / 48 GiB memory / unlimited disk**, and
  the Q16-HDRI build uses 8 bytes/pixel (6000×4000 RAW ≈ 768 MB pixel cache). With
  4 concurrent children this is exactly the oversubscription the comment claims to fix.
- **Change:** in the same `call_once` block, `::setenv("MAGICK_THREAD_LIMIT","1",1)`
  plus `MAGICK_MEMORY_LIMIT` (e.g. `256MiB`), `MAGICK_MAP_LIMIT`, `MAGICK_AREA_LIMIT`,
  `MAGICK_WIDTH`/`MAGICK_HEIGHT` — env vars are inherited across fork/exec.
  Alternative: prepend `-limit thread 1 -limit memory 256MiB` to every arg vector.
- **Verify:** `MAGICK_THREAD_LIMIT=1 magick identify -list resource` shows Thread: 1;
  during a scan, per-`magick` CPU stays ≤ 100%.

### QW-5. Move the TPM submit logging inside the lock ✅ ~5 lines

> **STATUS: ✅ DONE 2026-08-02 (validated).**
> `submit()` now captures `queued_for_type` under the mutex; the debug log uses
> the local instead of touching `type_to_queue_` unlocked. Regression stress
> test: `ThreadPoolManagerTest.ConcurrentSubmitWhileDrainingIsSafe` (8 threads ×
> 100 mixed-type submissions racing the drain path; trips under TSan on the old
> code, pins exactly-once execution on any build). Suite green (257 tests).
- **File:** `src/orchestration/thread_pool_manager.cpp:228-270`.
- **Problem (data race / UB):** the critical section ends at :261; :268 then calls
  `type_to_queue_[type]` (mutating `unordered_map::operator[]`, may rehash) with no
  lock, concurrently with `schedule()`'s locked mutations (:486, :436). Poco's
  `logger.debug(fmt, ...)` evaluates arguments unconditionally.
- **Change:** capture `size_t qsize = type_to_queue_[type].size()` inside the locked
  block; log after unlock using the local.
- **Verify:** TSan build of the unit tests (`test_scheduler_concurrency.cpp`) is clean.

### QW-6. Hoist `Poco::Logger::get` out of the comparison inner loops ✅ ~5 lines

> **STATUS: ✅ DONE 2026-08-02 (validated).**
> Cached accessors `similarityLogger()` / `finderLogger()` (magic statics)
> replace every `Poco::Logger::get("SimilarityCalculator"/"DuplicateFinder")`
> in both files — the per-pair global-mutex registry lookup is gone from the
> hot loops. New `SimilarityCalculatorTest` (5 tests) pins cosine semantics
> (identical=1, scale-invariant, orthogonal=0, opposite=-1, degenerate=0)
> through this and future hot-loop refactors (the suite caught a recursive
> static-init bug during development). Suite green (262 tests).
- **Files:** `src/media_processors/similarity/similarity_calculator.cpp:137`;
  `src/orchestration/duplicate_finder.cpp:231` (`areMetadataCompatible`).
- **Problem:** `Poco::Logger::get(name)` takes a **process-global mutex** and does a
  map lookup; it is called unconditionally once (or twice) per pair comparison —
  ~8.9e9 lock ops for a fresh 100k-photo run — serializing all worker threads.
- **Change:** `static Poco::Logger& logger = Poco::Logger::get("SimilarityCalculator");`
  at function/file scope (logger references are stable for process lifetime).
- **Verify:** profile a dedup batch; `Poco::Logger::get` disappears from the hot stack.

### QW-7. Set `busy_timeout` + per-connection pragmas on every pooled connection ✅ ~20 lines

> **STATUS: ✅ DONE 2026-08-02 (validated).**
> A `SqliteTunedSessionPool` subclass overrides Poco's `customizeSession` hook
> (invoked once per newly created physical connection) and applies
> `SessionManager::applySqlitePragmas`: busy_timeout=5000, synchronous=NORMAL,
> cache_size, temp_store, foreign_keys=ON. WAL stays a one-time (persistent)
> setting at initialize; the dead `PRAGMA cache = shared` line is gone; the
> per-lease `foreign_keys` round trip in `getConnectedSession` is gone. The
> previously dead config keys `database.sqlite.cacheSize`/`tempStore` are now
> wired through DatabaseManager into the pragmas. Tests:
> `SessionPragmasTest.EveryConcurrentConnectionIsTuned` (4 simultaneous leases,
> each connection asserted tuned) + `ApplySqlitePragmasHonorsParameters`.
> Suite green (264 tests).
- **File:** `src/database/session/session_manager.cpp:34-57` (init-time pragmas) and
  `:98-110` (`getConnectedSession`).
- **Problem:** pragmas are applied to **one** borrowed session at `initialize()`.
  `synchronous = NORMAL` and `cache_size` are per-connection, so the other 31 pool
  connections run defaults (`FULL` fsync per commit, 2 MB cache). `busy_timeout` is
  **never set** (default 0) → concurrent writers get immediate `SQLITE_BUSY`, and the
  ops layer swallows it (`scanned_files_ops.cpp:113-127` `catch(...){return false;}`
  and ~40 similar sites) → **silent write loss under load**. `PRAGMA cache = shared`
  at :48 is not a real pragma (silently ignored) — delete it. `foreign_keys = ON` is
  re-issued on *every lease acquisition* (:107) instead of once per connection.
- **Change:** add a `configureConnection(Poco::Data::Session&)` applying
  `busy_timeout=5000`, `synchronous=NORMAL`, `cache_size`, `temp_store=MEMORY`,
  `foreign_keys=ON`, optionally `mmap_size`, invoked once per **physical** connection
  (track configured connections, or configure on pool-miss creation). Remove the
  per-lease `foreign_keys` statement. Mind total cache: 32 × 20 MB = 640 MB — pick
  ~4–8 MB/connection.
- **Also:** wire or delete the dead config keys `database.sqlite.cacheSize` and
  `database.sqlite.tempStore` (declared `config_manager_factory.cpp:371-372`, read nowhere).
- **Verify:** `PRAGMA busy_timeout` returns 5000 on a leased session; hammer
  concurrent ejects + scan without lost writes.

### QW-9. Make user-initiated clear-cache a true reset (added 2026-08-02)

> **STATUS: ✅ DONE 2026-08-02 (validated).**

- **Problem found during QW-4 validation:** the ClearAllCaches RPC cleared the
  disk caches + cache DB tables, but (a) `MediaGenerationFailures` (the 3-strike
  negative cache) survived, so permanently failed files still answered
  FAILED_PRECONDITION until a server restart; (b) the viewer's session-lifetime
  full-res temp renders survived; (c) the SwiftUI client neither dropped
  `ImageCache.shared` nor re-requested anything, so the UI kept showing
  pre-clear pixels and the whole reset looked like a no-op.
- **Server:** `MediaGenerationFailures::clearAll()` (new) called from
  ClearAllCaches; `ProcessingErrorsOps::deleteBySource(db, "MediaGeneration")`
  (new, with `kDeleteProcessingErrorsBySource`) purges the persisted error rows;
  `fullResCache()`/`fullResMutex()` moved to file scope with a new
  `clearFullResCache()` (deletes temp JPEGs + empties the index) also invoked.
- **Client:** on RPC success `SettingsViewModel.clearAllCaches()` now clears
  `ImageCache.shared` and posts `.cachesCleared`; `ThumbnailView` observes it and
  resets its tile state (including stale "Unreadable" verdicts) and re-requests;
  `VurioDuplicatesView` reloads loaded rows so `cached_thumbnail_path` fields
  stop pointing at deleted files.
- **Tests:** `MediaGenerationFailuresTest` (4 tests: threshold semantics,
  success-reset, clearAll) and
  `ProcessingErrorsOpsTest.DeleteBySourceRemovesOnlyThatSource`.

### QW-8. Populate timestamps in the web preview handler's cache rows ✅ ~10 lines

> **STATUS: ✅ DONE 2026-08-02 (validated).**
> New `PreviewCacheOps::makeRecord(source, cached)` fills source mtime, cached
> file size, and created/last-accessed timestamps; ALL FIVE generation sites now
> use it (both web handler sites — previously writing zeroed records — plus the
> gRPC, media_processor, and media_backfill sites, which had three verbatim
> copies of the population logic, now deleted). Tests:
> `PreviewCacheOpsTest.MakeRecord_PopulatesFreshnessFields` (asserts the mtime
> matches what ingest computes) + `MakeRecord_MissingSourceYieldsZeroMtime`.
> Suite green (266 tests).
- **File:** `src/core/webserver/web_handlers_preview.cpp:158-161` and `:208-211`.
- **Problem:** `PreviewCacheRecord` is constructed with only `source_path`/`cached_path`;
  `source_modified_at` defaults to 0, so `media_processor.cpp:277`'s freshness check
  (`== source_mtime`) always fails → ingest re-transcodes RAWs that already have
  good previews. The gRPC path (`grpc_server_core.cpp:1533-1551`) and
  `media_backfill.cpp:124-147` populate correctly.
- **Change:** factor a `PreviewCacheOps::makeRecord(source, cached)` helper from the
  gRPC site; use it at all three call sites.
- **Verify:** request a preview over HTTP, then ingest the same file — no re-transcode.

---

## Phase 1 — Kill the DB pool contention at the source

This is the server-side root cause of frontend commit `2a8141d` ("VU wipes and
resets when day queries hit DB pool contention"). Measured: one `GetDayGroups`
page ≈ **1.3 s of DB work** under a single held lease.

### DB-1. Make the day-view queries index-only

> **STATUS: ✅ DONE 2026-08-02 (validated).**
> **Measured on a snapshot of the live 5.6 GB DB: day count 1568 ms → 92 ms
> (17×), day page 733 ms (review) / 167 ms (snapshot) → 90 ms.**
> Implemented as option 1: `is_video` VIRTUAL generated column
> (`VURIO_IS_VIDEO_EXPR`, instant ALTER, no rewrite) + the taken_day partial
> index predicate now includes `is_video = 0` + `VURIO_DAY_VIEW_FILTER` uses
> the column — the 14 per-row LIKEs are gone from all day-view queries.
> Migration is automatic at startup: `kAlterScannedFilesAddIsVideo` (guarded)
> and a one-time index rebuild when the existing definition lacks `is_video`
> (detected via sqlite_master text). Two DB-6 items pulled forward because the
> residual cost was the suppression subquery's full duplicate_members scan:
> covering index `idx_dm_rep_group_file(is_representative, group_id, file_id)`
> and startup `ANALYZE` (one-time when sqlite_stat1 missing; `PRAGMA optimize`
> afterwards). Migration cost at first startup: ~0.8 s index rebuild + ~0.6 s
> ANALYZE.
> **Decisions:** keyset paging NOT done — the VU scroller needs random access
> by day offset, and with the covering index the OFFSET walk is index-only and
> cheap. `total_days` caching deferred — at ~90 ms/query it's no longer the
> contention driver; revisit only if scroll bursts still show pool pressure.
> Tests: `DayGroupsOpsTest.DayQueriesAreServedByTheTakenDayIndex` (EXPLAIN
> plan pinned to the partial index) and `EnsureTableUpgradesLegacyTakenDayIndex`
> (legacy-index migration path). Suite green (268 tests). Pre-migration
> snapshot of the live DB retained in the session scratchpad.
- **Files:** `include/database/sql_constants.hpp:511-518` (`VURIO_NOT_VIDEO_P`,
  14 `lower(file_name) NOT LIKE` terms), `:527`, `:552` (`kSelectScanDaysCount`,
  measured 536 ms), `:557` (`kSelectScanDaysWithPagination`, 733 ms), `:605`, `:610`;
  `src/database/day_groups_ops.cpp:63-124`.
- **Problem:** `file_name` is not in `idx_scanned_files_taken_day`, so both day
  queries fetch all ~160k rows from a table averaging ~35 KB/row (see DB-2) and run
  ~2.2M `lower()`+LIKE evaluations, twice per page.
- **Change (pick one, first is best):**
  1. Add a generated column `is_video` (computed from `file_name` extension) and
     fold `is_video = 0` into the **partial index predicate** of
     `idx_scanned_files_taken_day` — video rows disappear from the index and the
     14 LIKE terms are deleted from the queries entirely. Only 14,129/160,319 rows
     are videos. Requires a one-time index rebuild migration.
  2. Minimum: composite/covering index
     `ON scanned_files(taken_day, file_name) WHERE processed >= 0 AND processed <> 3`.
- **Also in this task:**
  - Keyset-page the day list: `WHERE taken_day < :last ORDER BY taken_day DESC LIMIT :n`
    instead of OFFSET (stops sweeping/sorting all 3,498 distinct days per page).
  - Cache `total_days` in memory; invalidate on scan completion / eject / location
    change instead of recomputing the 536 ms `COUNT(DISTINCT)` per page.
  - In `day_groups_ops.cpp:63-124`, don't hold one lease across the whole page
    build (day list + per-day count + per-day files); take short leases per phase.
- **Verify:** `EXPLAIN QUERY PLAN` shows covering index / no row fetches;
  `GetDayGroups` page < 50 ms; fast VU scrolling no longer triggers
  "Timed out acquiring DB session from pool".

### DB-2. Evict the 2.83 GB write-only `exif_blob`

> **STATUS: ✅ DONE 2026-08-02 (validated).**
> **Live DB: 5.65 GB → 2.53 GB (−3.1 GB, 55%); integrity_check ok; day
> queries ~137 ms on the slimmed DB.** Chose "stop writing" over the side
> table: the blob is regenerable from the originals and MP-1 replaces EXIF
> extraction anyway. Code: `exif_blob` removed from `kUpsertScannedFile`,
> `kUpdateExifMetadata`, `ScannedFileRow`, and `updateExifMetadata`'s
> signature; the schema column survives as NULL. **Bonus: the second magick
> subprocess (`magick <file> exif:`) existed solely to produce this blob — a
> FULL decode of the original per ingested file — and is deleted outright
> (half of MP-1's per-file win landed early).** One-time offline migration
> executed on the live DB with the server stopped (UPDATE→NULL 3.3 s +
> VACUUM 15 s); pre-migration snapshot retained in the session scratchpad.
> Test: `ScannedFilesOpsTest.IngestWritesParsedExifButNeverTheBlob`.
> Suite green (269 tests).
- **Files:** written at `src/media_processors/image_processor.cpp:94-123` via
  `kUpsertScannedFile`/`kUpdateExifMetadata` (`sql_constants.hpp:149,285`).
- **Problem (measured):** `sum(length(exif_blob)) = 2,831 MB` across 92,151 rows —
  **half the database** — and no SELECT anywhere reads it (`scanned_files_ops.cpp:374`
  comments that it is deliberately excluded). It halves row density for every scan,
  and bloats WAL, `VACUUM INTO` backups (`database_manager.cpp:167`) and the VACUUM
  at `web_handlers_duplicates.cpp:1463`.
- **Change:** stop writing it, or move it to a side table
  `exif_blobs(file_path TEXT PRIMARY KEY, blob BLOB)`. Migration: create side table,
  copy, `UPDATE scanned_files SET exif_blob = NULL`, then `VACUUM` (or incremental
  vacuum) to reclaim ~2.8 GB. Note EXIF *fields* used by queries are separate
  columns — only the raw blob is dead.
- **Verify:** DB shrinks to ~2.8 GB; day-view and `listUnprocessed` timings drop
  proportionally.

### DB-3. Stop the duplicate finder's nested-lease churn

> **STATUS: ✅ DONE 2026-08-02 (validated).**
> New `loadFileArtifactsBatch(Session&, ids)` loads artifacts via chunked
> (500-id) `IN` queries with bulk extraction on the ALREADY-HELD session;
> `loadFileArtifacts` gained a `Session&` overload (old signature is now a
> thin lease-acquiring wrapper around it, sharing one row→artifact converter).
> `processBatch`'s representative and ungrouped loads both use the batch
> loader — the tens of thousands of nested pool checkouts (+ per-checkout
> statement) per finder batch are gone; the finder now holds exactly one
> lease during loading. `FileArtifact` + the loaders made public for direct
> testing. Per-file loads inside worker tasks (cross-group checking etc.)
> unchanged — those are DF-phase work. Tests:
> `BatchArtifactLoaderMatchesSingleLoader` (byte-identical embeddings incl.
> NULL bytes — the historical truncation trap; missing-artifact ids skipped)
> and `BatchArtifactLoaderSpansChunkBoundary` (510 ids across the 500 chunk).
> Suite green (271 tests).
- **Files:** `src/orchestration/duplicate_finder.cpp:283` (batch-scope lease),
  `:355-385` (per-representative loads), `:394-423` (per-ungrouped loads),
  `:842-957` (`loadFileArtifacts` — acquires **its own lease** at :846 per call).
- **Problem:** tens of thousands of pool checkouts per batch (each with a
  `PRAGMA foreign_keys` round trip, see QW-7) while the batch already holds a lease —
  2 slots consumed, and this is the background pressure that starves day queries.
- **Change:** add a `loadFileArtifacts(Poco::Data::Session&, ...)` overload (pattern
  already exists: `ScannedFilesOps::updateFilePath(Session&, ...)` at
  `scanned_files_ops.cpp:1315`) and pass the held session down. Replace the per-id
  loop with set-based loads: `WHERE sf.id IN (...)` chunks of ~500 joined to
  `image_artifacts`. (Full algorithmic fix is Phase 3; this alone removes the pool
  pressure.)
- **Verify:** during a dedup batch, pool lease count stays ≤ 2 for the finder;
  day queries stay responsive while dedup runs.

### DB-4. Fix `SessionManager` acquire path

> **STATUS: ✅ DONE 2026-08-02 (validated).**
> All six items landed: (1) the slot is reserved under `lease_mutex_` and the
> pool checkout happens OUTSIDE it (decrement-on-throw); (2) waiters block on
> `lease_cv_.wait_until` — the 50 ms sleep-poll and its backoff knob are gone
> (`database.session.acquireBackoffMs` removed from factory/config/server
> wiring); (3) `Lease` holds `std::optional<Session>` and its dtor returns
> the session to the Poco pool BEFORE freeing the counted slot; (4)
> `acquire_timeout_ms_` is atomic; (5) dead `SessionLease` class deleted
> (files + all CMake references) along with the one-arg counter-corrupting
> `Lease(SessionManager&)` ctor; (6) `ensureSessionManagerReady` guarded by a
> mutex. Tests (`SessionAcquireTest`): CV-driven wake on release with a
> usable session, timeout honored on exhausted pool, 8-thread churn over a
> 2-slot pool with zero lost slots. Suite green (274 tests).
- **File:** `src/database/session/session_manager.cpp:64-134`.
- **Problems:**
  1. `:71-79` — `getConnectedSession()` (real SQL round trip; may open a file-backed
     connection) is called **while holding `lease_mutex_`**: every DB op in the
     process serializes through one mutex across disk I/O.
  2. `:94` — waiters sleep-poll 50 ms; `lease_cv_` is notified at :122 but never
     waited on. 0–50 ms latency tax per contended acquire.
  3. `:134` — `Lease::~Lease` calls `releaseLease()` **before** the member session
     is destroyed/returned to the Poco pool → another thread can pass the counter
     gate and hit `SessionPoolExhaustedException`.
  4. `include/database/session_manager.hpp:47-48` — `setAcquireTimeoutMs`/`setBackoffMs`
     write plain ints read concurrently (config-monitor thread vs workers) → make atomic.
  5. Dead/dangerous code: `src/database/session/session_lease.cpp:8` binds a session
     copied out of a destroyed temporary `Lease` (counter corruption if ever used —
     currently zero callers); the one-arg `Lease(SessionManager&)` ctor (:126-127)
     never increments the counter but the dtor always decrements. Delete both.
  6. `src/database/database_manager.cpp:114-142` — `ensureSessionManagerReady()` is
     an unlocked check-then-act; guard with a mutex or `std::call_once`.
- **Change:** reserve the slot under the lock → unlock → `getConnectedSession()`
  (decrement on throw); replace the sleep-poll with
  `lease_cv_.wait_until(lk, deadline, pred)`; in `~Lease` explicitly return the
  session (`session_ = Poco::Data::Session()`-style or reorder members) **then**
  `releaseLease()`.
- **Verify:** under a scroll burst + active dedup, acquire latency is CV-immediate;
  no `SessionPoolExhaustedException` in logs.

### DB-5. Parameterize the string-concatenated `IN` clause (injection + breakage)

> **STATUS: ✅ DONE 2026-08-02 (validated).**
> **Resolved by deletion, not parameterization:** investigation showed the
> entire cluster was dead code — `executeFilteredCount`, its helper
> `getRegisteredLocationKeys`, the four unusable `kCount*Filtered` constants
> (`IN (?)` can never bind a list) and `kGetRegisteredLocationKeys` had ZERO
> production callers. All deleted, removing the injection surface outright
> (a tombstone comment in sql_constants.hpp records why). Two tests that
> exercised only the dead helper were trimmed. New positive test:
> `ScannedFilesOpsTest.LocationKeysWithApostrophesRoundTrip` pins that the
> LIVE location-scoped paths (upsert/getByPath/getFilesByLocationKey/
> getLocationKey) handle apostrophe-bearing keys via bound parameters. A
> repo scan confirms no other quote-concatenation into SQL remains in
> src/database. Suite green (275 tests).
- **File:** `src/database/scanned_files/scanned_files_ops.cpp:1026-1041`
  (`executeFilteredCount`), constants `kCountScannedFilesFiltered` family at
  `sql_constants.hpp:246-253` (written as `location_key IN (?)` which can never
  bind a list — the concatenation exists to work around that).
- **Problem:** `in_clause += "'" + registered_keys[i] + "'"` with **no escaping**;
  keys are user-supplied filesystem paths (`mediaLocation:` settings). A path with
  an apostrophe (e.g. `/Volumes/Sapna's iPad`, which exists in this library) yields
  malformed SQL, silently swallowed by `catch(...){}` at :1050 → wrong counts.
  Anything worse is plain SQL injection.
- **Change:** build `IN (?,?,...)` with N placeholders and bind each key. Also drop
  `getRegisteredLocationKeys`' per-key `information`-level logging (:966-998) on
  this hot count path.
- **Verify:** register a location containing `'` in the path; counts are correct.

### DB-6. Indices, ANALYZE, and remaining query gaps

> **STATUS: ✅ DONE 2026-08-03 (validated).**
> (Items 2's covering dm-index and 4's ANALYZE landed early in DB-1.)
> **Measured on the live-DB snapshot:** unprocessed sweep (runs EVERY
> scheduler tick) 320 ms full scan → ~0 ms via new partial index
> `idx_scanned_files_unprocessed` (predicate textually identical to
> `kListUnprocessed`); BINNED pagination → covering SEARCH on new composite
> `idx_duplicate_groups_mode_rep_date` (zero-selectivity legacy
> `idx_duplicate_groups_mode` dropped on sight by ensureTables); item 3
> (`kSelectDuplicateGroupsForFile`) needed NO new index — post-ANALYZE the
> planner already runs it as a MULTI-INDEX OR (0.6 ms).
> **Bug found & fixed while wiring:** the scanned_files index-creation block
> reused one named Poco `Statement` across executes, which silently drops
> later statements — the new index never got created. Switched to the
> per-statement `sess <<` pattern (a comment marks the trap); verified
> against the live DB that no OTHER index is missing from the remaining
> multi-statement blocks. Tests:
> `ScannedFilesOpsTest.UnprocessedSweepIndexPredicateMatchesTheQuery`
> (textual index/query predicate invariant — plan choice is size-dependent,
> so EXPLAIN on a small test DB can't guard this — plus sweep behavior) and
> `DuplicateFinderRangeTest.GroupPaginationUsesCompositeModeDateIndex`
> (legacy index gone, covering search, no TEMP B-TREE). Suite green
> (277 tests).
- **Changes:**
  1. Partial index for the unprocessed sweep: `kListUnprocessed`
     (`sql_constants.hpp:280`) currently plans a full table SCAN and runs **every
     scheduler tick** (`media_processor.cpp:825`) — in steady state scanning
     everything to return nothing. Add
     `CREATE INDEX ... ON scanned_files(id) WHERE processed = 0 OR processed = -99 OR (processed >= -100 AND processed < 0)`
     (match the exact predicate in the SQL).
  2. `CREATE INDEX ... ON duplicate_groups(mode, representative_created_date)` —
     kills the `USE TEMP B-TREE FOR ORDER BY` (83 ms/page measured) in
     `kSelectDuplicateGroupsWithPaginationByMode` (`:450`). Drop the useless
     single-value `idx_duplicate_groups_mode` (`SELECT mode, count(*)` → one value).
  3. Index `(mode, representative_file_id)` for `kSelectDuplicateGroupsForFile`
     (`:622`) — currently a 34,900-row scan per call inside purge loops
     (`grpc_server_core.cpp:1130`, `web_handlers_duplicates.cpp:1324`).
  4. Run `ANALYZE` / `PRAGMA optimize` at startup or shutdown — `sqlite_stat1` does
     not exist in the live DB; the planner is flying blind.
- **Verify:** `EXPLAIN QUERY PLAN` on each query; `sqlite_stat1` exists.

### DB-7. Transactions, batching, N+1s

> **STATUS: ✅ DONE 2026-08-03 (validated).**
> All six sub-items landed:
> 1. `removeByLocationKey`/`completelyRemoveById` restructured — cache paths
>    read first, pure-DB transaction commits, unlinks run AFTER; both
>    wrong-session rollbacks fixed (rollback now on the transaction's session).
> 2. Scanner batching: `ScannedFilesOps::upsertBatch` (one transaction/WAL
>    commit per batch) + FilesManager accumulates new-file rows (500/batch,
>    flushed per root/pass/exit, per-row fallback if a batch fails). The
>    changed-file "hard approach" path stays immediate (rare, ordering-
>    sensitive). Scanner callback verified single-threaded.
> 3. `getGroupsWithMembers`: one set-based members query per page (IN over
>    integer group ids, ORDER BY group_id, similarity DESC) instead of one
>    statement per group.
> 4. `markProcessedWithEscalation`: single CASE UPDATE (was full-row read +
>    second statement on every failure path); escalation semantics pinned by
>    test.
> 5. HTTP thumbnail cache hits no longer write `last_accessed_at` per hit
>    (matches gRPC; MP-2 owns access-time tracking when eviction goes LRU).
> 6. `loadEjectionPaths` cached (5 s TTL — changes only via settings UI);
>    `GetServerStatus`'s five scanned_files COUNTs collapsed into one
>    aggregated query/lease (`getStatusCounts`, poll runs every 2 s).
> Tests: `UpsertBatchWritesAllRowsAtomically` (incl. conflict semantics),
> `MarkProcessedWithEscalationSemantics` (fresh/escalate/recover/missing),
> `StatusCountsMatchIndividualCounts` (agreement + absolute values),
> `GroupsWithMembersBatchedFetch` (distribution + similarity ordering);
> existing `RemoveByLocationKeyCascade` covers the restructured deletes.
> Suite green (281 tests).
- **Changes:**
  1. **Filesystem I/O inside transactions:** `scanned_files_ops.cpp:152-242`
     (`removeByLocationKey`-family: begin → loop of thumbnail/preview
     `exists/remove` → commit) and `:263-332` (`completelyRemoveById`) hold the
     single SQLite writer lock across thousands of unlinks. Restructure: read the
     cached paths in a short transaction, commit DB deletes, unlink **after**.
     Both catch blocks (:248-251, :337-340) also acquire a *new* lease and call
     `rollback()` on the wrong session (a no-op) — the real transaction unwinds
     mid-flight. `bulkEject` (`web_handlers_duplicates.cpp:478-695`) already does
     all of this correctly — copy its discipline.
  2. **Scanner batching:** `files_manager.cpp:91,157,230,258` — per-file
     `getByPath` + `upsert`, each with its own lease and implicit commit; a 160k
     rescan = 320k checkouts + up to 160k WAL commits. Batch 500–1000 upserts per
     transaction with a reused prepared statement. Single biggest scan-throughput win.
  3. **Members N+1:** `duplicate_groups_ops.cpp:939` (`getGroupsWithMembers`)
     issues one member query per group; one `WHERE group_id IN (...)` for the page
     is ~1 ms (measured) vs ~100 prepare/execute cycles.
  4. `markProcessedWithEscalation` (`scanned_files_ops.cpp:578`): collapse
     read-then-update to one
     `UPDATE ... SET processed = CASE WHEN processed < 0 AND ? < 0 THEN ? - 100 ELSE ? END`.
  5. HTTP thumbnail cache **hits** do a DB write (`updateAccessTime`,
     `web_handlers_thumbnail.cpp:150`) — a write flood during scrolls. Batch access
     times in memory and flush periodically (the gRPC path at
     `grpc_server_core.cpp:1363` already skips it).
  6. Cache `loadEjectionPaths()` (`grpc_server_core.cpp:412`) with config-change
     invalidation instead of a full `user_settings` scan + lease per list RPC;
     collapse `GetServerStatus`'s 5 leases/5 queries (`:190-200`) into one.

---

## Phase 2 — Concurrency correctness (crashes, deadlocks, lost work)

### CC-1. `recreateThreadPool` deadlock

> **STATUS: ✅ DONE 2026-08-03 (validated).**
> Took the plan's preferred option: **runtime pool recreation is deleted
> entirely** (function + declaration; a tombstone comment records the
> deadlock mechanics). `tpm.thread.idleTimeoutSeconds` changes now log
> "takes effect after server restart" (Poco pools cannot change idle time
> live — matches the DatabaseManager pool-size precedent) and the value is
> stored under the mutex; `tpm.thread.stackSizeKB` is applied LIVE via
> `pool_->setStackSize()` (affects only future threads — no recreation
> needed). The silent queue-discard is gone with the function. Test:
> `ChangingThreadSettingsMidFlightNeverStallsOrDropsWork` drives the exact
> trigger — config changes while tasks run and the queue is non-empty —
> and asserts nothing stalls and all queued work completes (the old code
> hangs this test). Suite green (281 tests).
- **File:** `src/orchestration/thread_pool_manager.cpp:624-651`; triggered from
  `onConfigChange` at :752 (`tpm.thread.idleTimeoutSeconds`) and :773
  (`tpm.thread.stackSizeKB`) — i.e. any config.yaml edit touching these.
- **Problem:** waits on `cv_` (no timeout) holding `mutex_`, then destroys the Poco
  pool (joins workers) still under `mutex_`. Completing workers re-enter
  `schedule()` → block on `mutex_` → their runnables never finish → pool join never
  returns. **Hard deadlock of every TPM entry point.** Also `:635-636` silently
  clears all queued work on an unrelated config edit.
- **Change:** don't recreate the pool at runtime — log "requires restart" (the
  pattern `DatabaseManager` already uses at `database_manager.cpp:77`). If runtime
  recreation must stay: bounded `wait_for`, and never destroy the pool while
  holding `mutex_`.

### CC-2. Moved-from task re-queued on `NoThreadAvailableException`

> **STATUS: ✅ DONE 2026-08-03 (validated).**
> `FunctionRunnable::takeFn()` (new) reclaims the callable from a
> never-started runnable; all THREE start-failure catches
> (NoThreadAvailable / std::exception / ...) now re-queue an intact
> `QueuedTask{fn, file_path}` and stop the pass — the generic paths
> previously DROPPED the task outright. The queue-front copy also became a
> move. Testability seam: `startOnPool(Poco::Runnable&)` protected virtual
> (single Poco crossing point; dtor made virtual). Test:
> `FailedStartRequeuesTaskIntact` injects the throw and asserts the work
> still executes AND the file_path isn't zombie-blocked — under the old
> code the re-queued shell ran as a swallowed bad_function_call.
> Suite green (282 tests).
- **File:** `thread_pool_manager.cpp:522-577`.
- **Problem:** `task.fn` is moved out at :524; the catch at :565 re-queues `task`
  (now holding an **empty** `std::function`) at :576. When later scheduled it throws
  `bad_function_call`, swallowed at :363 → silent work loss, and the stale
  `file_path` blocks resubmission via the dedup check (:235-241).
- **Change:** only move out of the task after `pool_->start` succeeds (construct
  runnable from the task, and on failure move the fn back / re-queue the intact task).
  Also surface (log at warning + re-queue or notify caller) the drop paths at :579
  and :589.

### CC-3. Dedup fan-out must not block a pool slot / be starved by UI tasks

> **STATUS: ✅ DONE 2026-08-03 (validated).**
> Took the strongest of the plan's options: **both comparison fan-outs
> (processBatch + performCrossGroupChecking) now run on a bounded, JOINED
> local worker set** (`runFanOut`: ≤4 threads, abort_ checked between items)
> — never on the TPM. This removes the entire deadlock/starvation class
> (same-type parent+children, latch with no timeout, pause-hang) rather than
> patching it; the join also keeps the tasks' stack-reference captures safe,
> which is why bail-early latch patches were NOT viable. The
> TaskCompletionNotifier latch machinery is deleted; the checkpoint now
> withholds on `abort_` (an id-watermark advanced past aborted ids would
> skip them forever). UI prioritization softened: background types are
> THROTTLED to one running task while UI tasks queue, not skipped — scanning
> /dedup keep trickling under continuous scrolling. Tests:
> `BackgroundTypeIsThrottledNotStarvedByUIBacklog` (old code fails: bg task
> never starts under a UI backlog); the fan-out rewrite is exercised by the
> existing finder suite. Suite green (283 tests).
- **Files:** `duplicate_finder.cpp:701-710` and `:1790-1798` (submit N children to
  the *same* TPM type, then `latch_cv.wait` with no timeout);
  `thread_pool_manager.cpp:481-484` (whenever any thumbnail/preview/fullres task is
  queued, **all** non-media types are skipped); media cap
  `media_generation_cap_ = 4`.
- **Problem:** while the user scrolls (steady thumbnail demand), dedup children are
  never scheduled and the parent blocks forever holding a slot. Combined with QW-3's
  broken abort, this was unrecoverable.
- **Change:** (a) give both latch waits a deadline + `abort_` predicate; (b) run the
  dedup driver on its own dedicated thread rather than a TPM slot, or process the
  fan-out with a bounded local worker set; (c) change the UI-priority rule from
  "starve everything" to a share/ratio so background types retain ≥1 slot.
- **Verify:** scroll VU continuously during a dedup run; dedup progresses.

### CC-4. SchedulerService races and lifecycle

> **STATUS: ✅ DONE 2026-08-03 (validated).**
>
> **Regression found & fixed 2026-08-03 (field report: "processing stops at
> ~0.5k after registering a location"):** the dispatch lambda submitted to the
> TPM is the ONLY resetter of a job's RUNNING state — and it lives in a queue
> that `clearQueues()` (DeregisterLocation, integrity check, cache reset)
> wipes, and that `submit()` rejects while draining. A dropped dispatch left
> the job RUNNING forever; every later tick logged "already running, skipping"
> at DEBUG. The user's deregister→re-register ate the queued `mediaProcessor`
> dispatch, so no file was ever routed again (the "0.5k processed" was the
> scanner's filter-ignored files — the UI counts state 3). Fix: a
> `DispatchDropGuard` rides in the lambda's captures and restores IDLE via a
> lock-free CAS on a `weak_ptr<Job>` when the callable is destroyed without
> executing (`jobs_` became `shared_ptr<Job>`; NO `jobsMutex_` in the guard —
> its destructor can run inside the TPM mutex while `triggerJob` holds
> `jobsMutex_`→TPM-mutex, ABBA otherwise). Regression test:
> `SchedulerConcurrencyTest.DroppedDispatch_ResetsJobToIdle` (pause pool →
> trigger → clearQueues → job must be triggerable and run again).
>
> All seven sub-items landed (scheduler_service.cpp rewritten):
> 1. `Job::interval/backoff` → atomic `interval_ms`/`backoff_ms`.
> 2. `registerJob` rejects duplicate ids (was: destroyed a live Job under its
>    own worker — UAF + std::terminate).
> 3. `stop()` moves jobs out under `jobsMutex_` and joins OUTSIDE it.
> 4. Dispatch claims IDLE→RUNNING via `compare_exchange_strong`
>    (`executeJobIfIdle`), used by both the worker loop and `triggerJob` —
>    the TOCTOU double-dispatch window is gone.
> 5. TPM job lambdas capture `weak_ptr<SchedulerService>`
>    (`enable_shared_from_this`) instead of raw `this`; expired ⇒ skip state
>    recording. Test constructions switched to `make_shared` accordingly.
> 6. The three per-job 50 ms sleep-poll loops replaced with `wakeCv_`
>    waits (stop/resume/unregister notify) — 60 wakeups/s gone, stop is
>    immediate even mid-interval or while paused.
> 7. `shutdownCallback_`/`pauseCallback_` set/copied under `callbackMutex_`,
>    invoked outside it.
> Tests: `DuplicateRegistrationIsRejected`, `TriggerWhileRunningIsRejected-
> Atomically`, `StopIsPromptWithLongIntervalsAndWhilePaused`; the existing
> scheduler-concurrency / jobs-independence suites exercise the rewrite.
> Suite green (286 tests).
- **File:** `src/orchestration/scheduler_service.cpp`.
- **Fixes:**
  1. `job.interval`/`job.backoff` written unlocked at :274/:167-181 vs locked
     readers (:225, :381) → make them `std::atomic<int64_t>` ms.
  2. `registerJob` on an existing id (:112) destroys a live `Job` whose worker
     thread holds a pointer into it (UAF + `std::terminate` from joinable dtor) →
     reject duplicate ids.
  3. `stop()` (:38-55) joins workers while holding `jobsMutex_`, which the TPM job
     lambda (:420-444) also takes → join outside the lock (move `unique_ptr`s to a
     local first; `unregisterJob` :190-201 already does this right).
  4. TOCTOU on `JobState` (:152 unlocked vs :330/:403) allows double-dispatch →
     `state.compare_exchange_strong(IDLE, RUNNING)`.
  5. Job lambdas capture raw `this` (:411) and can run after the owner is reset
     during shutdown (scheduler stopped at `server_server_shutdown.cpp:84` *before*
     the TPM drain at :130, and the drain has a 10 s timeout) → capture a
     `std::weak_ptr` (make `SchedulerService` shared / `enable_shared_from_this`)
     or drain the TPM before clearing jobs.
  6. Replace the three per-job 50 ms sleep-poll loops (:135-146) with
     `condition_variable::wait_until` (removes 60 wakeups/s and shutdown latency).
  7. `shutdownCallback_`/`pauseCallback_` (:117-118) written post-start from main
     thread, read from workers → set before `start()`, or guard.

### CC-5. Shutdown lifetime: make `DatabaseManager` a real `shared_ptr`

> **STATUS: ✅ DONE 2026-08-03 (validated).**
> `DatabaseManager` is now genuinely `std::shared_ptr`-owned end to end:
> ServerInitializer/Server members, ServerShutdown signature, and
> `GrpcServerCore::start` all take/hold real shares; **every no-op-deleter
> `shared_ptr(raw, [](…){})` laundering site is deleted** (both initializer
> aliases, the ThumbnailHandler construction, the integrity-check wrapper).
> `MediaServiceImpl`/`AdvancedSettingsServiceImpl` ctors take the shared_ptr
> (keeping a raw `db_` convenience pointer for their bodies); their
> fire-and-forget generation lambdas and the detached integrity thread now
> capture OWNING shares — abandoned drain-timeout tasks can no longer
> dereference a freed DB. Integrity-check thread hardened: terminal
> `catch (...)` (a non-std exception used to call std::terminate and leave
> is_running=true + scheduler paused forever) and the unbounded 5 Hz drain
> loop bounded at 60 s. Detached launch kept — now lifetime-safe since every
> argument owns its dependency. Verified by compile-time enforcement plus the
> gRPC media-service socket tests exercising the new ownership path.
> Suite green (286 tests).
- **Files:** `src/core/server_server_initializer.cpp:194` and `:325`
  (`std::shared_ptr<DatabaseManager>(raw, [](DatabaseManager*){})`), copied again at
  `grpc_server_core.cpp:1388`; `src/core/server.cpp:352` (`database_manager_.reset()`)
  vs `:355` (`std::_Exit`).
- **Problem:** no-op-deleter aliases hand "shared" ownership that isn't. TPM tasks
  abandoned by the 10 s drain timeout, and the **detached** integrity-check thread
  (`web_handlers_duplicates.cpp:1211-1215`), can dereference the DB after :352.
- **Change:** own `DatabaseManager` as `std::shared_ptr` in `ServerInitializer`,
  pass it by value everywhere; delete the no-op-deleter constructions. Convert the
  integrity-check thread from detached to owned/joined (it also needs `catch(...)` —
  a non-std exception currently calls `std::terminate` and leaves
  `state_.is_running=true` + scheduler paused forever; and its drain loop at
  :1296-1300 needs a timeout).

### CC-6. Smaller concurrency items (batch into one PR)

> **STATUS: ✅ DONE 2026-08-03 (validated).**
> All items landed except two deliberately skipped:
> - **TPM:** `kill_timeout_` → atomic; `has_ui_tasks_fn` uses `find()` (no
>   phantom deque insertion); `TaskCleanupHelper` is stack-scoped (was
>   thread_local: guard deferred to THREAD exit, stale state across tasks);
>   submit dedup is an O(1) `inflight_paths_` set ("type|path", maintained in
>   submit/finish/cancel/clearQueues) replacing the O(queue)+O(active) scan
>   under mutex_ per submit.
> - **FilesManager:** `running_` reset by scope guard + `catch (...)` — a
>   non-std exception can no longer permanently disable scanning.
> - **Initializer:** mediaProcessor job typeKey "fileScan" → "media_processor"
>   (copy-paste fix; now matches the share MediaProcessor configures).
> - **Thumbnail generation locks:** per-key map (erase-while-held race +
>   leaked entries) → fixed 64-stripe mutex array.
> - **HEIC:** `makeTempFilePath` (shared racy mt19937 gone), hard 60 s
>   timeout via runProcessWithTimeout (was untimed wait), `-Z 2560` so HEIC
>   previews respect the QHD cap.
> - **subprocess_runner:** adaptive 1→50 ms poll backoff (−~25 ms/spawn).
> - **gRPC:** `Shutdown(now+5s)` deadline; `GrpcServerCore::stop` detaches
>   the finder's group-event callback BEFORE tearing down the service it
>   points at; `DuplicateFinder::group_event_cb_` set/copied under a mutex.
> - **MediaReadyBus:** per-subscriber queue bounded at 1024, drop-oldest
>   (events are invalidation hints; the client re-fetches).
> - *Skipped:* image_task.cpp retry sleeps (dead code — CL-2 deletes the
>   file); ConsoleInputManager signal-handler rework deferred to CL/AR
>   phases (touching signal semantics without live shutdown testing is
>   riskier than the bug).
> Suite green (286 tests); one test-target source list fixed (heic's new
> deps). Existing TPM/scheduler suites exercise the changed paths.
- `ConsoleInputManager::stop()`/`waitForShutdown()` unlocked concurrent join/reset
  (`server_console_input_manager.cpp:163-200`); signal handler uses
  async-signal-unsafe Poco logging and `std::_Exit(0)` on SIGINT bypassing graceful
  shutdown (:353-368).
- `FilesManager::runOnce` has no `catch(...)` — a non-`std::exception` escape leaves
  `running_=true` forever, permanently disabling scanning
  (`files_manager.cpp:272-364`) → scope-guard the reset.
- `mediaProcessor` job registered with `typeKey = "fileScan"`
  (`server_server_initializer.cpp:389`) — copy-paste; give it its own type.
- Thumbnail per-file generation lock erased while held
  (`web_handlers_thumbnail.cpp:244-285`) → concurrent double-generation; unlock
  before erase or use a striped fixed mutex array (also fixes unbounded map growth).
- `heic_adapter.cpp:25-31`: shared unguarded `std::mt19937` → racy, colliding temp
  names; use `src/utils/temp_paths.cpp` helper. Also add `-Z 2560` to the sips args
  (currently full-res "previews") and use `runProcessWithTimeout` (currently
  untimed `handle.wait()`).
- `subprocess_runner.cpp:60`: 50 ms poll adds ~25 ms mean latency per `magick` spawn
  (~25 min across the live cache) → adaptive backoff (1→2→4…50 ms) or kqueue
  `EVFILT_PROC`.
- `image_task.cpp:9-11`: 500/1000 ms in-thread retry sleeps park pool slots → if
  `ImageTask` stays (see CL-2, it is currently dead code), requeue-with-delay instead.
- gRPC: `server_->Shutdown()` with no deadline (`grpc_server_core.cpp:1732`) →
  pass a deadline; `setGroupEventCallback` captures a raw service pointer freed at
  :1734 → guard/clear callback before `services_` teardown; `MediaReadyBus`
  subscriber queues are unbounded (`include/core/media_ready_bus.hpp:36`) → cap + drop policy.
- TPM misc: unlocked config fields `kill_timeout_`/`idle_timeout_seconds_`
  (:730-750) → atomics; `has_ui_tasks_fn` uses `operator[]` (:435-437) → `find()`;
  `thread_local TaskCleanupHelper` (:353) → plain stack object; O(queue) dedup scan
  per submit (:232-251) → maintain an `unordered_set<type+path>` side index.

---

## Phase 3 — Dedup algorithm overhaul (hours → minutes; slider → sub-second)

Current state (verified): single `EMBEDDING` mode; 512-dim float32 embeddings
(2 KB/photo) in `image_artifacts` keyed by **path**; cross-batch matching is a
linear scan; sqlite-vec `VectorIndex` exists but is effectively unused; a
threshold change wipes all groups and reprocesses from id 0.

### DF-1. Load once, compare cheap (prerequisite for everything else)

> **STATUS: ✅ DONE 2026-08-03 (validated).**
> All three items landed:
> 1. **EmbeddingArena** — one contiguous `vector<float>` of L2-normalized
>    rows keyed by file_id, populated ONLY in single-threaded preloads (run
>    start, batch preload, cross-group preload); the CC-3 fan-outs strictly
>    read. `computeSimilarity` is a bare `dotSimilarity` when both files are
>    in the arena, with the legacy blob-cosine as fallback (zero-norm /
>    mixed-dim / out-of-run artifacts). Arena memory released at run end.
> 2. **Run-level pool (`RunState`)** — representatives + ungrouped files load
>    ONCE per findDuplicates run (previously reloaded from the DB every
>    batch) and are mutated in memory as files group; batch artifacts and
>    cross-group member artifacts are set-based preloads on a held session.
> 3. Dead `FileArtifact` fields (phash/features/features_method) removed.
> **Note:** `ImageArtifactsOps::upsertEmbedding` L2-normalizes the blob at
> write time and feeds the SAME normalized vector to the vector index; the
> arena still re-normalizes on load so legacy (pre-normalization) rows stay
> valid, and cosine-metric KNN is normalization-invariant either way.
> Tests: `SimilarityCalculatorTest.DotSimilarityMatchesLegacyCosine` /
> `L2NormalizeProducesUnitNorm` / `L2NormalizeRejectsZeroVector` pin the
> primitives; new `FindDuplicatesGroupsSimilarEmbeddingsEndToEnd` runs the
> REAL pipeline (run-state load → arena preload → local-worker fan-out →
> group creation → checkpoint) over deliberately unnormalized fabricated
> embeddings and asserts grouping + checkpoint advance. (Possible only
> because CC-3 removed the TPM from the fan-out.)
> **Test-infra caveat discovered:** a `:memory:` DatabaseManager is broken
> under the connection pool — every pooled connection is its own empty DB.
> The end-to-end test uses a file-backed temp DB; existing `:memory:`
> fixtures survive only by connection-reuse luck (worth migrating when
> touched). Suite green (290 tests).

- **Files:** `duplicate_finder.cpp:342-430` (per-batch pool reload),
  `similarity_calculator.cpp:72-140` (`bytesToFloat32` copies, per-pair
  re-normalization in double).
- **Changes:**
  1. Load representatives + ungrouped pool **once per run** (not per batch) into a
     contiguous arena: one `std::vector<float>` (100k × 512 = 200 MB) + parallel id
     array. Batched set-based SELECTs (DB-3). Mutate in memory as files get grouped.
  2. **L2-normalize embeddings at write time** (`ImageArtifactsOps::upsertEmbedding`)
     so similarity is a bare float dot product; view blobs via
     `reinterpret_cast<const float*>` instead of copying; drop the per-pair `sqrt`s.
  3. Drop dead `FileArtifact` fields `phash`, `features`, `features_method`
     (`include/orchestration/duplicate_finder.hpp:126-139`).
- **Expected:** 10–40× on the inner loop (with QW-6), DB time per run from minutes
  to <10 s, −230 MB/batch of realloc churn.

### DF-2. Make the vector index real and primary

> **STATUS: ✅ DONE 2026-08-03 (validated).**
> (Status block reconstructed post-hoc — the DF-2 commit `b6c4253` shipped
> the code but missed this file.) All changes landed:
> - **`VectorIndex::reconcile()`** at startup: set-based backfill of
>   embeddings that predate the index + purge of vectors for deleted files;
>   idempotent, fresh-DB tolerant, cost proportional to drift.
> - **`remove()`/`removeMany()`** wired into all three file-deletion paths —
>   deleted files no longer poison KNN results.
> - **KNN primary:** ONE distance-filtered query per new file
>   (`distance <= 1 - threshold + eps`) drove both the representative search
>   and the cross-batch candidates; the silent full-scan fallback on empty
>   intersection was removed (only an empty KNN result — an index fault —
>   warns). Superseded in shape by DF-3's edge discovery, which keeps the
>   same principle: vec0 is EXACT KNN under the cosine metric, so
>   score = 1 − distance with no embedding loads.
> - **Tests:** `FindDuplicatesGroupsViaKnnIndexEndToEnd` (skips when
>   vec0.dylib absent) pins KNN-driven grouping == linear-scan grouping.
> - **Deferred:** per-thread vec0 handles / in-memory hnswlib if the single
>   VectorIndex mutex ever shows up at scale.

- **Files:** `src/database/vector_index.cpp`; insert site
  `image_artifacts_ops.cpp:78-82`; consumer `duplicate_finder.cpp:435-437, 520-531`;
  `findCrossBatchCandidates` `:1406-1475` (never consults the index).
- **Problems:** (1) no backfill — libraries embedded before `database.vector.enabled`
  have an empty index; (2) `VectorIndex::remove` has **zero call sites** — deleted
  files poison KNN results; (3) the KNN result is intersected with representatives
  and silently **falls back to full scan** when the intersection is empty — the
  common case at scale; (4) the O(n²) ungrouped-vs-ungrouped path bypasses the
  index entirely; (5) one object-wide mutex serializes every query (:168).
- **Changes:** startup backfill from `image_artifacts`; call `remove()` on file
  deletion; use KNN as the primary candidate generator for **both** representatives
  and ungrouped files with a distance filter (`distance <= 1 - threshold_min`)
  instead of top-k-then-intersect; never full-scan when the index is ready.
  For real scale consider an in-memory hnswlib index (~215 MB at 100k, 0.05–0.2 ms
  per query) in front of/instead of vec0 (vec0 is brute-force: ~200 MB read per
  query at 100k), or at minimum per-thread sqlite handles to drop the global mutex.
- **Expected:** 4.45e9 → ~2e7 comparisons for a fresh 100k run.

### DF-3. Union-find grouping over a persisted edge list; sub-second slider

> **STATUS: ✅ DONE 2026-08-03 (validated).**
> The finder is now two decoupled phases; ~850 lines of greedy/repair
> machinery deleted (`duplicate_finder.cpp` 2061 → ~1220 lines).
> - **Discovery:** `processBatch` records pair scores >= the discovery floor
>   (`duplicates.edges.floor`, default 0.80 = the UI slider's loosest
>   position, clamped <= the similarity
>   threshold) into new table `duplicate_edges(file_a, file_b, score)`
>   (WITHOUT ROWID, PK (a,b), score index). KNN path: score = 1 − distance
>   directly (vec0 is exact under cosine) — neighbor embeddings never load.
>   Fallback path (no index): once-per-run pool of all embedded files,
>   arena dot products, each new-new pair recorded from the higher-id side
>   only. Edges land in ONE transaction per batch; the id watermark advances
>   as before (withheld on abort).
> - **Clustering:** `rebuildGroupsFromEdges` = union-find (path halving +
>   union by size) over edges `>= similarity threshold`; components >= 2
>   become groups; representative via the existing purity logic; member
>   `similarity_score` = max incident edge (rep = 1.0). **Group-id
>   stability** for the SwiftUI client: a component containing an existing
>   group's representative (or any previous group head) reuses that id;
>   the whole rewrite (update reused + insert new + delete vanished groups
>   and orphan members) is ONE transaction. Timing logged.
> - **Slider:** a similarity change now just sets `regroup_requested_` —
>   NO group wipe, NO checkpoint reset, NO embedding math; the next finder
>   tick (5 s) re-clusters from stored edges in milliseconds. Successive
>   ticks coalesce into one flag (natural debounce). Only a threshold BELOW
>   the edge floor falls back to the legacy full reset (edges cleared too).
>   A purity-order change also schedules a re-cluster (rep choice depends
>   on it).
> - **Deleted:** `getGroupIdForFile`, `areMetadataCompatible` (was already
>   inert — see DF-5), `findMatchingGroup`, `createDuplicateGroup`,
>   `addToGroup`, `trySplitAndRedistribute`, `findCrossBatchCandidates`,
>   `performCrossGroupChecking`. Group events are no longer emitted during
>   rebuild — the UI catches up via its 2 s status polling.
> - **Hygiene:** `purgeOrphanEdges()` at run start drops edges whose
>   endpoints left `scanned_files` (deletion paths untouched).
> - **Deferred (item 5):** the checkpoint stays a single id watermark;
>   re-embedded files below it are still never re-compared. Revisit with an
>   `artifact_updated_at` cursor or dirty flag if/when re-embedding becomes
>   a real flow.
> - **Migration:** a pre-edge-list install (advanced watermark, groups, but
>   an empty `duplicate_edges`) gets its checkpoint reset once at
>   `initialize()` so discovery repopulates the edges; groups stay visible
>   throughout and the first rebuild reuses their ids via rep matching.
> - **Tests:** `ThresholdDecreaseSchedulesReclusterWithoutWipe` (groups +
>   checkpoint survive a slider move), `ThresholdBelowEdgeFloorTriggersFullReset`,
>   `EmptyEdgeTableBehindWatermarkResetsCheckpoint` (migration), and
>   end-to-end `SliderReclusterFromEdgesEndToEnd`: three files (two at
>   cos ~0.9997, one at ~0.93 to both), group at 0.95 = {a, a2}, 3 edges on
>   disk, slide to 0.92 → SAME group id grows to {a, a2, m} with the
>   watermark untouched. Obsolete `CrossGroupCheckingExecutes` removed.
>   Suite: 328 tests, 296 passed / 32 pre-existing skips / 0 failed.
> - **Validate:** `./restart -b`; let a scan finish, then drag the tightness
>   slider — groups should re-form near-instantly (look for
>   "Re-clustered from N edge(s) in X ms" in the log) and the UI should NOT
>   go through a wipe-and-rebuild flash. New files should keep joining
>   groups incrementally.

- **Files:** greedy clique builder `duplicate_finder.cpp:718-807` (worst-case ~m³,
  single-threaded); first-match-wins ordering `:1417-1471`; repair passes
  `trySplitAndRedistribute` `:1252-1404` and `performCrossGroupChecking`
  `:1611-1801` (O(low-sim members × groups) with a DB load per task, run after
  **every** non-empty batch — ~3e8 comparisons/run at scale); slider handler
  `:1511-1547` (wipes groups + resets checkpoint on any similarity change, no
  debounce, no `running_` check — 20 slider ticks = 20 wipes + 20 overlapping full
  recomputes).
- **Changes:**
  1. Persist pair scores once above the *loosest* supported threshold:
     `pair_scores(file_a, file_b, score)` built from the KNN pass.
  2. Grouping = threshold the edge list + **union-find** (optionally tightened by a
     similar-to-representative pass). Deterministic, order-independent, O(E·α(n)).
  3. A slider move becomes `WHERE score >= ?` + re-cluster: **milliseconds, no
     embedding math, no group wipe**. Debounce config-driven recompute (2–5 s
     trailing) and require the finder idle (`abortCurrentFind` + wait) before any
     destructive rebuild.
  4. Delete `trySplitAndRedistribute` and `performCrossGroupChecking` once
     union-find lands (they exist to repair greedy grouping's mistakes).
  5. Fix the checkpoint model: currently a single id watermark
     (`:125-126, 286-293`) — re-embedded files below the watermark are never
     re-compared, and a dropped child task discards the whole batch (:810-828).
     Use a `dedup_dirty` flag or `artifact_updated_at` cursor.
- **Note:** keep group-id stability in mind for the SwiftUI client (it keys UI
  state on group ids).

### DF-4. Embedding throughput (ONNX)

> **STATUS: ✅ DONE 2026-08-04 (validated).**
> Items 1-3 + the ensureTable hoist landed; 4-5 deferred (below).
> - **Reduced decode (item 1):** `IMREAD_REDUCED_COLOR_4` — libjpeg decodes
>   the 2560px preview at 1/4 scale (640px) directly, 4-16x cheaper
>   preprocessing and ~1/16th peak pixel memory; falls back to a full decode
>   when the reduced image would undershoot the model input (tiny sources).
> - **CoreML EP (item 2):** config-gated (`media.image.onnx.coreml.enabled`
>   — code default OFF so tests stay CPU; config.yaml turns it ON), appended
>   as `AppendExecutionProvider("CoreML", {MLComputeUnitsAll, MLProgram,
>   ModelCacheDirectory=dedup_server/data/coreml})`. Any failure downgrades that session
>   to CPU-only; CoreML itself falls back per-node for unsupported ops. The
>   compiled-model cache avoids recompiles across pooled sessions/restarts.
> - **Session plumbing (item 3):** ONE process-wide `Ort::Env`
>   (`OnnxSessionWrapper::sharedEnv()`, was one env per pooled session);
>   `ORT_ENABLE_ALL` (was BASIC); the model's I/O layout (names, dims,
>   types, image-input index/NHWC, output name) is introspected ONCE at
>   session creation into `OnnxSessionWrapper::IoSpec` — the adapter
>   previously re-queried and re-allocated all of it on EVERY inference.
> - **Adapter dedup:** the two ~300-line near-identical ComputeEmbedding
>   overloads now share decode/preprocess/inference helpers (~250 lines
>   gone); the memory path's std::cerr spam replaced with logger calls.
> - **ensureTable hoist:** `QualityPipeline` ensured `image_artifacts` once
>   per IMAGE (lease + sqlite_master probe); now once per database via a
>   memoized `persistEmbedding` helper (stale-memo safe: failed upsert
>   re-ensures and retries once).
> - **Config-key bug found & fixed:** the session manager read
>   `media.image.quality.onnx.sessionCache.*` but factory/config.yaml define
>   `media.image.onnx.sessionCache.*` — the cache knobs were silently inert
>   (defaults happened to match). Manager + tests now use the real keys.
> - **DF-3 follow-up (floor vs. slider range):** the UI slider's loosest
>   position is threshold 0.80 (distance 0.20) — BELOW the 0.85 edge floor,
>   so the slider's bottom band would have forced legacy wipes. Floor
>   default is now 0.80 (`duplicates.edges.floor` registered + in
>   config.yaml), and a new `duplicate_edges_meta(floor)` provenance row
>   makes floor changes safe: edges recorded at a higher floor than
>   configured trigger a one-time re-discovery (groups stay visible);
>   below-floor threshold drops also pull the floor down and record it.
> - **Test-infra:** migrated the DuplicateFinderRangeTest fixture off
>   `:memory:` to a file-backed temp DB (the longer initialize() shifted
>   pooled-connection rotation and the luck ran out — 4 tests failed until
>   migrated, exactly as predicted in the DF-1 caveat).
> - **Deferred (item 4, batching):** 8-16 images per Run() needs cross-image
>   orchestration in MediaProcessor and weakens per-file error isolation;
>   revisit only if throughput is still short after CoreML.
> - **Deferred (item 5, fp16/int8):** model asset change, not a code change.
>   QualityPipeline ensureTable-per-image (the "Also" item) IS done.
> - **Tests:** reduced-decode dims (large 2000x1600 → 500x400, mid 600x800
>   full fallback, small full), `SharedEnvAndCachedIoSpec` (real model,
>   skips if absent), `ComputeEmbeddingEndToEnd` (real model through the
>   full rewritten path: 512-float non-zero embedding). Suite: 333 tests,
>   301 passed / 32 pre-existing skips / 0 failed.
> - **Validate:** `./restart -b`, ingest new photos and watch the log for
>   "Created ONNX session (CoreML EP)" (first session creation compiles the
>   model — expect a one-time delay, then cached). Embedding throughput
>   (files/min in processing) should rise noticeably vs. before; a repeat
>   restart should reuse `dedup_server/data/coreml`. Expect one-time
>   "Edge list unusable at floor 0.800 (stored floor -1.000...)" +
>   re-discovery on first start (the 0.85→0.80 floor migration).

- **Files:** `onnx_session_manager.cpp:18-24`; `onnx_adapter.cpp:59, 75-144,
  170-298`; `quality_pipeline.cpp:56,121,182`; config
  `media.image.onnx.sessionCache.maxSessions: 4`.
- **Changes (ranked by gain):**
  1. Decode reduced: `cv::IMREAD_REDUCED_COLOR_4` (target is 224×224; current code
     full-decodes the 2560px preview then downscales) → 4–16× preprocessing, peak
     per-image memory ~270 MB → ~5 MB.
  2. Append CoreML execution provider with CPU fallback (none is configured —
     currently pure CPU on Apple Silicon) → 3–8× inference.
  3. One process-wide `Ort::Env` (currently one per session); cache input/output
     names/shapes in the wrapper (currently re-queried + allocated per inference);
     `ORT_ENABLE_ALL` instead of `BASIC`.
  4. Batch 8–16 images per `Run()` (shape currently hard-wired to batch 1).
  5. Consider fp16/int8 model: 350 MB fp32 × 4 pooled sessions = 1.4 GB resident.
- **Also:** `QualityPipeline` calls `ImageArtifactsOps::ensureTable` per image
  (`quality_pipeline.cpp:70,142,195`) → hoist to init.

### DF-5. Optional: cheap pre-filter

> **STATUS: ✅ DONE 2026-08-04 (validated).**
> **Decision: DELETE, not revive.** With the vec0 exact-KNN index primary
> (DF-2) and persisted edges (DF-3), a Hamming pre-filter would only
> accelerate the rare no-index linear fallback — and reviving dHash would
> ADD per-photo ingest work, the opposite of MP-1's direction.
> - **Deleted (~1,050 lines):** `opencv_adapter.cpp/hpp`
>   (`ComputePhash`, both overloads), `features_adapter.cpp/hpp` (the
>   BALANCED-mode sibling, zero callers, not even tested),
>   `test_phash_quality.cpp` (15 tests of the dead subject),
>   `upsertPhash`/`upsertFeatures` warn-stubs +
>   `ImagePhashRecord`/`ImageFeaturesRecord`, and every CMake reference.
> - **areMetadataCompatible fate:** the function itself died in DF-3; this
>   step removed the leftover inert config surface — the five finder fields
>   + initialize() reads, the five `duplicates.metadata.*` factory
>   registrations, their config.yaml keys, and their
>   config.descriptions.json entries (10). The extension-parsing crash and
>   RAW↔JPEG-tolerance contradiction died with the code.
> - CL-2's `features_adapter`/`ComputePhash`/`duplicates.metadata.*` items
>   are hereby done; the rest of CL-2 remains.
> - **Tests:** suite 318 tests (was 333 — the 15 PhashQualityTest cases
>   left with their subject), 286 passed / 32 pre-existing skips / 0 failed.
> - **Validate:** `./restart -b` — behavior is identical by construction
>   (everything removed was unreachable from production); grep confirms
>   zero references to the deleted symbols remain.

- pHash plumbing is dead (`OpenCvAdapter::ComputePhash` has no production callers;
  `upsertPhash`/`upsertFeatures` are warn-and-return-false stubs). Either delete it
  (see CL-2) or resurrect a 64-bit dHash stored beside the embedding as a Hamming
  pre-filter (rejects ~99% of pairs at ~2 ns each). Also `areMetadataCompatible`
  (`duplicate_finder.cpp:223-274`): reads `aspect_ratio_tolerance_`/
  `dimension_tolerance_` but never uses them (TODO at :269 — the config keys are
  inert); `substr(find_last_of('.'))` throws on extensionless paths when
  `require_same_format_` is on; the 20% size tolerance contradicts RAW↔JPEG purity
  pairing — resolve deliberately.

---

## Phase 4 — Media pipeline & caching

### MP-1. Cut per-photo ingest work (currently 4 subprocess spawns, 2 full RAW demosaics)

> **STATUS: ✅ DONE 2026-08-04 (validated).**
> Per-RAW ingest is now ONE subprocess and ZERO full demosaics (when the
> camera embedded a preview JPEG, i.e. virtually always):
> - **EXIF without pixel decode (item 1):** new `ExifReader` backend.
>   RAW → LibRaw `open_file()` metadata-only (make/model/capture
>   time/output dims incl. 90° flip handling) — no unpack, no demosaic, no
>   subprocess; the old `magick identify` full-demosaiced every RAW just to
>   report dimensions. Non-RAW → `identify -ping` (header/EXIF read only —
>   the old call decoded the whole JPEG). `image_processor.cpp`'s 70-line
>   inline subprocess block replaced by the call; tier-2 low-res filter
>   unchanged.
> - **Thumbnail in-process (item 2, reinterpreted):** the plan's
>   `-write`-merge is unnecessary — `ThumbnailGenerator::generateToBlob`
>   now encodes JPEG/PNG inputs (previews, RAW-embedded JPEGs, ordinary
>   photos) with OpenCV in-process: reduced decode when profitable,
>   fit-to-box resize matching magick's "SxS" geometry (incl. upscale
>   parity), quality-controlled `imencode`, EXIF orientation honored by
>   imread, metadata-free by construction. No subprocess, no temp output
>   file (~4 ms vs a ~100-300 ms spawn). magick remains the fallback and
>   the TIFF/HEIC/exotics path. ONNX already reads the preview in-process
>   (DF-4), so preview decode-count is: transcode(1) + embed(¼-scale) +
>   thumb(¼-scale) — no full re-decodes.
> - **Scaled decode for large JPEGs (item 3):** `buildPreviewMagickArgs`
>   emits `-define jpeg:size=5120x5120` (2× the 2560 preview target)
>   BEFORE jpeg inputs — libjpeg decodes at the smallest sufficient scale,
>   2-4× faster on big camera JPEGs; non-jpeg inputs unaffected.
> - **Spawn accounting:** RAW: 3 spawns + 1-2 demosaics → 1 spawn +
>   0 demosaics. Large JPEG: 3 spawns (one full-decode identify) →
>   1 faster spawn + 1 cheap `-ping` spawn. Small JPEG: 2 spawns →
>   1 cheap `-ping` spawn.
> - **Caveat noted:** the OpenCV thumbnail path ignores ICC profiles
>   (wide-gamut sources render slightly less saturated at 256px); our own
>   previews are already sRGB-converted at transcode, so this only touches
>   direct thumbnails of wide-gamut originals.
> - **Tests:** arg-shape tests updated + new (`-define` before jpeg input,
>   absent for non-jpeg), `ExifReaderTest` (ping dims, missing file),
>   `ThumbnailFastPathTest` (blob decodes back at 256 long edge with
>   aspect preserved; small-source upscale parity). Suite: 324 tests,
>   292 passed / 32 pre-existing skips / 0 failed.
> - **Validate:** `./restart -b`, ingest a batch with RAWs and watch
>   processing throughput; log should show no `magick identify` storms,
>   and thumbnails should appear noticeably faster after processing
>   (log line "Generated thumbnail in-process (OpenCV)").
- **Files:** `src/media_processors/image_processor.cpp:36-50` (`magick identify` on
  the original — a **full RAW decode**), `:97-105` (`magick <raw> exif:<tmp>` —
  another full decode); `pipeline_thumbnail_helper.cpp:90` + `thumbnail_generator.cpp:118`
  (re-decode of the QHD preview for the 128px thumb).
- **Changes:**
  1. Replace both EXIF subprocess calls with a library read — LibRaw's
     `imgdata.other` is already populated by the embedded-thumbnail extraction step,
     or use exiv2. Kills the 2 gratuitous RAW demosaics.
  2. Merge thumbnail + ONNX input into the preview transcode:
     `magick in -write thumb128.jpg -resize 224x224 onnx.jpg` (one decode instead
     of three).
  3. For large JPEG inputs add `-define jpeg:size=<2×target>` before the input file
     (libjpeg scaled decode, 2–4×).
- **Expected:** per-RAW ingest cost roughly halved; spawns 4 → 2 (→ 1 with #2).

### MP-2. DiskCache: incremental accounting + DB-driven LRU eviction

> **STATUS: ✅ DONE 2026-08-04 (validated).**
> Full DiskCache rework; every plan item landed:
> - **In-memory index:** `index_` (path → size + insert time) built ONCE at
>   initialize() via one recursive scan; every insert/delete/eviction
>   maintains it incrementally. The per-insert readdir + per-file stat +
>   sort (~180 ms at 59k files, under the mutex) is gone.
> - **Copies outside the mutex:** copy/move/stream operations are now
>   3-phase — (locked) admission + eviction + destination claim via a
>   `pending_writes_` guard; (UNLOCKED) the multi-MB file copy; (locked)
>   index update. The mutex is held for microseconds, not for I/O — the
>   ~5 thumbnails/s ceiling is gone.
> - **DB-driven LRU eviction:** injectable hooks
>   (`setEvictionHooks(lru_query, evicted_notify)`); the initializer wires
>   thumbnail_cache/preview_cache `ORDER BY last_accessed_at` queries in
>   (new `ThumbnailCacheOps`/`PreviewCacheOps::lruCachedPaths` +
>   `deleteByCachedPaths`, chunked parameterized IN). Evicted files' rows
>   are deleted in the same pass — no more dangling rows / regeneration
>   storms. Insert-order fallback (seeded from mtimes at startup) covers
>   hook-less use and shortfalls.
> - **Batched eviction:** frees the overflow + 10% of the limit as
>   headroom, so eviction doesn't re-trigger on every subsequent insert.
> - **Underflow guard:** all size decrements clamp at 0 (`indexEraseLocked`)
>   — the wrap that projected an astronomical size and nuked the cache
>   can't happen.
> - **MediaBackfill split-brain fixed:** `installHook` now captures the
>   initializer's REAL cache instances (hook registration moved to after
>   cache construction); the backfill no longer builds a second DiskCache
>   pair over the same directories.
> - **Sharding + collision-free naming:** cached files live at
>   `<2-hex>/<stem>_<16-hex-hash-of-FULL-path><ext>` — 256 shards keep
>   directory fan-out flat at 59k+ files, and hashing the full source path
>   ends the silent basename collisions. (One shard level, not the plan's
>   `ab/cd/`: two levels would mean ~1 file/dir at current scale.)
>   Existing flat files are still indexed/evicted; lookups go through DB
>   rows, so no migration is needed.
> - **Behavior change:** `deleteFromCache` now also refuses files that are
>   mid-write or marked in use (previously unguarded).
> - **Tests:** all 24 existing DiskCacheTest cases pass (one rewritten:
>   same-basename sources must NOT collide anymore + a deterministic
>   same-source re-cache test), plus new `EvictionHooks_LruOrderAndNotification`
>   (hook-designated LRU file evicted first, owner notified of exactly the
>   deleted paths) and `ShardedLayout_ClearAndAccountingWork`. Suite: 327
>   tests, 295 passed / 32 pre-existing skips / 0 failed.
> - **Validate:** `./restart -b`; browse enough thumbnails to push the
>   cache toward its limit — eviction log lines now read
>   "Evicted N file(s), freed M MB (LRU)" and evicted thumbnails should
>   regenerate on demand without storms. New cache files appear under
>   2-hex shard subdirectories.

- **File:** `src/filesmanager/disk_cache.cpp`.
- **Problems:** every insert runs `enforceSizeLimit` → full `readdir` + per-file
  `stat` + sort (**~180 ms for 59k files, measured**) under `cache_mutex_`, which is
  also held across multi-MB file copies (:253, :333, :467, :513) — effective
  ceiling ~5 thumbnails/s. Eviction is FIFO by mtime (the DB's `last_accessed_at`
  LRU data is ignored) and never deletes `thumbnail_cache`/`preview_cache` rows →
  dangling rows → regeneration storms. `deleteFromCache` (:498) decrements
  `current_size_bytes_` with **no underflow guard** (wraps → total cache nuke via
  the projected-size path). `MediaBackfill` (`media_backfill.cpp:72-73`) builds a
  **second** DiskCache over the same directories → split-brain accounting (why the
  cache is over its limit) and void `files_in_use_` protection.
- **Changes:** in-memory index (path → size/atime) built at `initialize()`,
  maintained incrementally; evict by `SELECT cached_path FROM thumbnail_cache ORDER
  BY last_accessed_at LIMIT n` deleting file + row in one transaction; batch
  eviction to 10% headroom; move filesystem work outside the mutex; underflow guard
  at :498; plumb the initializer's real cache instances into `MediaBackfill::run`.
- **Also:** shard the flat cache directory (59k files in one dir; scales 1:1 with
  library) — `ab/cd/<hash>_<size>.jpg` on the first hex chars, and make
  `generateCacheFilename` (:713-718) hash the full source path (current
  basename-only naming silently collides).

### MP-3. Close the `size=0` thumbnail hole

> **STATUS: ✅ DONE 2026-08-04 (validated).**
> - **0 dropped from `VALID_SIZES`** — `isValidSize(0)` is now false, so the
>   web thumbnail gate 400s size=0 requests. `getClosestValidSize` snaps
>   small requests to real sizes: the LIVE bug was the gRPC default
>   `size=80` normalizing to 0 → full-resolution renders cached forever
>   that `kSelectBestFitThumbnail` could never serve. 80 → 128 now; 0 and
>   negatives → the 256 default.
> - **`ThumbnailGenerator::kNativeResolution` sentinel** for the two
>   full-res viewer paths (`grpc_server_core.cpp` full-res viewer,
>   `web_handlers_preview.cpp` `size=full`) — renders at source resolution,
>   bypasses size snapping, and by construction never touches the
>   thumbnail cache (both paths stream/hold the result themselves).
> - **Startup purge:** `ThumbnailCacheOps::purgeSizeZeroThumbnails`
>   deletes the poisoned `thumbnail_size=0` rows and the initializer
>   unlinks their files — the accumulated ~300 KB-each dead weight goes
>   away on first start.
> - **Tests:** `SizeZeroIsNotAValidCachedThumbnailSize`,
>   `SmallSizeRequestsSnapToRealSizesNotNative` (pins 80→128, 0/neg→256,
>   5000→1024); the native-args test now uses the sentinel. Suite: 329
>   tests, 297 passed / 32 pre-existing skips / 0 failed.
> - **Validate:** `./restart -b`; first start logs
>   "Purged N size=0 full-resolution thumbnail(s)" if any existed; grid
>   thumbnails at default sizes render as before; the full-size viewer
>   still shows native resolution.

- **Files:** `include/utils/thumbnail_generator.hpp:68` (`VALID_SIZES` includes 0),
  `thumbnail_generator.cpp:109` (skips `-resize` when ≤0), `:243-265`
  (`getClosestValidSize` returns 0 for requests ≤64), gate at
  `web_handlers_thumbnail.cpp:121-125`.
- **Problem (live evidence):** `size=0` passes validation and produces
  full-resolution "thumbnails" cached forever (`thumbnails_cache/*_0.jpg`, 300 KB
  each) that `kSelectBestFitThumbnail` (`sql_constants.hpp:671-674`,
  `thumbnail_size >= ?`) can never serve. Dead weight + full-res render vector.
- **Change:** drop 0 from `VALID_SIZES`; give the two native-resolution callers
  (`grpc_server_core.cpp:1453`, `web_handlers_preview.cpp:87`) an explicit
  `kNativeResolution` sentinel that bypasses the thumbnail cache.

### MP-4. Bound the full-res viewer cache

> **STATUS: ✅ DONE 2026-08-04 (validated).**
> - The unbounded `unordered_map` is now `FullResLruCache`
>   (`include/utils/full_res_lru_cache.hpp`, extracted so it's unit
>   testable): **LRU with 32-entry AND 512 MB caps**, exactly as the plan
>   sized it. Eviction unlinks the temp render; so do staleness detection
>   (the old code dropped the map entry but LEAKED the file when the
>   source's mtime changed), same-key replacement (same leak), QW-9
>   clear-caches, and the destructor at shutdown — no more ~5 GB of
>   invisible quality-95 temp JPEGs from scrolling the viewer.
> - Call sites simplified to `getFresh` (validates existence + mtime,
>   touches recency) / `put` — the three hand-rolled lock+find+check
>   blocks in `grpc_server_core.cpp` collapsed.
> - **Known small window (pre-existing):** a temp path already handed to
>   the client can be evicted before the client opens it; the client
>   retries via the normal UNAVAILABLE flow, same as QW-9's clear.
> - **Tests:** `FullResLruCacheTest` — entry-cap LRU order + unlink,
>   byte-cap eviction, stale-mtime unlink, replacement unlink, destructor
>   cleanup. Suite: 334 tests, 302 passed / 32 pre-existing skips /
>   0 failed.
> - **Validate:** `./restart -b`; zoom through many RAW photos in the
>   viewer, then check the temp dir (`ls /var/folders/.../T/fullres_*` or
>   wherever makeTempFilePath points) — at most ~32 renders should exist,
>   and they disappear on server shutdown.

- **File:** `grpc_server_core.cpp:1403-1462` (`fullResCache()` function-local static
  map + temp JPEGs).
- **Problem:** unbounded; nothing deletes the quality-95 native-res temp files
  during process lifetime (macOS only clears `/var/folders` on reboot). Scrolling
  the viewer through 500 photos leaves ~5 GB of temp files invisible to any cache
  limit.
- **Change:** LRU cap (e.g. 32 entries / 512 MB), `std::filesystem::remove` on
  eviction and on shutdown.

### MP-5. gRPC payload shape (do **not** change the transport model)

> **STATUS: ✅ DONE 2026-08-04 (validated).**
> Transport model untouched, as prescribed.
> - **Item 1 (the 4 MiB failure) fixed:** `GrpcClient.swift` now sets
>   `maximumReceiveMessageLength = 64 MiB` (grpc-swift default is 4 MiB).
>   The viewer timeline's `files_per_day=0` request survives a
>   bulk-imported library whose mtime-fallback dates pile ~17k files onto
>   one day — 64 MiB ≈ 280k files of headroom, and the transport is a
>   local unix socket so the ceiling costs nothing. Swift app builds.
> - **Per-RPC mount normalization (from item 2):** `mountedFilesystems()`
>   normalizes at collection; `isPathAvailable` compares directly — the
>   old code re-normalized every mount for EVERY file of a list RPC
>   (mounts × 17k files per GetDayGroups).
> - **Deferred (rest of item 2 — the proto slimming):** int64 ids,
>   response-level `thumbnail_cache_root` + relative paths, float
>   similarity. Deliberate: (a) changing a field's wire type on the same
>   field number is proto3-incompatible — both binaries must flip
>   atomically; (b) the SwiftUI client keys UI state on string ids in
>   many views, so the churn spans files with in-flight edits
>   (ServerImageLoader has uncommitted work); (c) the payload win is
>   bandwidth over a LOCAL unix socket — the only real failure mode was
>   the receive limit, now closed with 16x headroom. Revisit only if a
>   list RPC ever shows up in profiles.
> - **Tests:** `MediaAvailabilityTest` (mounts normalized at collection,
>   availability against normalized mounts). Suite: 336 tests, 304
>   passed / 32 pre-existing skips / 0 failed.
> - **Validate:** `./restart -b`, rebuild/run the app; the viewer
>   timeline should load even on days with huge file counts, and day-list
>   scrolling should feel no worse (mount work per RPC dropped).

- The paths-over-unix-socket design (`MediaPathResponse`, no image bytes on the
  wire, no base64) is correct — keep it.
- **Changes:**
  1. `GetDayGroups` with `files_per_day = 0` (used by the SwiftUI viewer timeline,
     `ViewerViewModel.swift` `filesPerDay=0, limit=3`) can exceed the Swift client's
     default **4 MiB** receive limit (~17k files × ~230 B; mtime-fallback
     `taken_time` can pile a bulk-imported library onto one day). Fix any of:
     set `maximumReceiveMessageLength` in `GrpcClient.swift:26`; cap
     `files_per_day` server-side (~500) with viewer paging via `GetDayFiles`.
  2. Slim `DayFile`/`DuplicateFile` (`grpc_server_core.cpp:381-457`): `id`/`group_id`
     as `int64` not strings; send `thumbnail_cache_root` once per response with
     relative `cached_thumbnail_path`; `float` similarity; pre-normalize the mount
     list and ejection roots **once per RPC** instead of per file
     (`media_availability.cpp:117-131` currently normalizes every mount per file).
     ~40–50% smaller list payloads. Requires proto change + Swift regen — coordinate
     both sides.

---

## Phase 5 — Architecture: one service layer, one API surface

Sequencing inside this phase matters: **AR-1 → AR-2 → AR-3**.

### AR-1. Break the three upward edges into the legacy HTTP layer

> **STATUS: ✅ DONE 2026-08-04 (validated).**
> All three named edges broken:
> - **(a) `ThumbnailService`** (`src/media_processors/thumbnail_service.*`):
>   the generation core (incl. the CC-6 striped locks, now process-wide
>   across every surface) extracted from the HTTP ThumbnailHandler. gRPC
>   and the backfill construct the service directly — no more full HTTP
>   request handler built inside a TPM task; the HTTP handler delegates.
> - **(b) `IntegrityCheckService`**
>   (`src/orchestration/integrity_check_service.*`): state + snapshot +
>   the detached runner (CC-5 lifetime/bounded-drain fixes preserved
>   verbatim) moved out of `DatabaseIntegrityCheckHandler` statics. gRPC
>   calls `Orchestration::IntegrityCheckService::start/status`; the HTTP
>   handler is plumbing that delegates to the same service.
> - **(c) Dead WebSocket broadcasts deleted** in `grpc_server_core.cpp`
>   (preview generation) and `pipeline_thumbnail_helper.cpp` —
>   `active_sockets_` is always empty in production; `MediaReadyBus`
>   serves the live client. `media_processors/` now has ZERO `core/web`
>   includes.
> - **One edge deliberately remains, marked for AR-2:** gRPC still
>   includes `web_handlers_duplicates.hpp` for `bulkEject`/`bulkUndoEject`
>   — exactly the shared gRPC/HTTP clone AR-2 extracts next; the include
>   carries an AR-2 comment and dies with that step.
> - **Tests:** `ThumbnailServiceTest.GenerateAndCacheEndToEnd` (real DB +
>   DiskCache + JPEG: generate, DB row, repeat-call cache hit, decode-back
>   box check); all handler/gRPC suites that compile the touched TUs still
>   pass. Suite: 337 tests, 305 passed / 32 pre-existing skips / 0 failed.
> - **Validate:** `./restart -b`; thumbnails/previews generate as before
>   from the app, and Advanced Settings → database integrity check runs
>   with live progress and completes (it now runs through the
>   orchestration service on both surfaces).

- **Files:** `grpc_server_core.cpp:26-34` (includes of `core/web/*`), `:1388`
  (constructs an HTTP `ThumbnailHandler` inside a TPM task to call
  `generateAndCacheThumbnail`), `:1204,1216` (integrity-check via static state on
  an HTTP handler class, `include/core/web/web_handlers_duplicates.hpp:261-262`),
  `:1560` + `pipeline_thumbnail_helper.cpp:144` (broadcast to
  `WebSocketStatusHandler::active_sockets_` — **always empty in production**).
- **Changes:** (a) move `generateAndCacheThumbnail` into a
  `src/media_processors/thumbnail_service.{hpp,cpp}`; HTTP handler and gRPC both
  call it. (b) move `IntegrityCheckState` + runner into
  `src/orchestration/integrity_check_service.{hpp,cpp}` owned by `ServerInitializer`
  (pairs with CC-5's join/timeout fixes). (c) delete the two dead WebSocket
  broadcast calls — `MediaReadyBus` already serves the live client.
- **Result:** `core/web` has zero inbound edges; media-pipeline TUs stop
  transitively including orchestration/config headers.

### AR-2. Extract shared services for the drifted gRPC/HTTP clones — keep the HTTP logic, it is the correct fork

> **STATUS: ✅ DONE 2026-08-04 (validated).**
> - **`src/services/eject_service.{hpp,cpp}`** created (namespace
>   `MediaDedup::Services`) holding: `VerifiedMoveResult` +
>   `copyFileVerified`/`moveFileVerified`, `BulkOpResult`,
>   `bulkEject`/`bulkUndoEject` (moved verbatim from
>   web_handlers_duplicates), `ejectErrored`/`undoEjectErrored`/
>   `ejectIgnored`/`undoEjectIgnored` (the HTTP fork — collision-safe
>   `name_1/` folders, SHA1-verified copies, transactional DB updates with
>   file rollback, pre-existing-row + eject-history cleanup — moved in and
>   de-duplicated into one parameterized core), and `purgeEjected` (merged:
>   gRPC's complete /ignored_ coverage + HTTP's member-count sync).
> - **All three data-loss bugs fixed** by deletion of the gRPC clones
>   (~500 lines): collision overwrites, counted-successful failed DB writes
>   that still deleted sources, skipped row/history cleanup. Both surfaces
>   now marshal only (`fillBulkReply` / `sendBulkResult`).
> - **gRPC now has ZERO `core/web` includes** — AR-1's last marked edge is
>   gone. `web_handlers_reset_errors.cpp`: 756 → 198 lines.
> - **Tests repointed at the service** (survive AR-3):
>   `test_eject_collision` drives `Services::bulkEject/bulkUndoEject/
>   moveFileVerified/copyFileVerified` directly (HTTP mocks deleted);
>   `test_purge_ejected_errored`'s hand-rolled purge loops replaced with
>   the real `Services::purgeEjected`; `test_duplicates_reset_api` already
>   tested the ops layer. The legacy hpp keeps `using`-aliases so the HTTP
>   surface compiles until AR-3 deletes it.
> - **Deferred to AR-3 (with reasoning):** the listing / preview-generation
>   / server-status / reset / clear-caches pairs. None carries a named
>   correctness divergence (the data-loss trio was all in the eject
>   family); their heavy logic already lives in the shared Ops layer, and
>   AR-3 deletes the HTTP copies wholesale — extracting services for them
>   now would only create indirection AR-3 immediately strands.
> - **Suite:** 338 tests, 306 passed / 32 pre-existing skips / 0 failed.
> - **Validate:** `./restart -b`; from the app run: eject errored files,
>   undo; eject ignored, undo; eject selected duplicates with two
>   same-named files (collision folders appear), undo-all; purge ejected —
>   all through gRPC, all now on the safe code path.

- **Evidence of drift with data loss** (gRPC `EjectErrors` at
  `grpc_server_core.cpp:634-770` vs HTTP at `web_handlers_reset_errors.cpp:96-274`;
  same four deltas again in `EjectIgnored` `:876-1013` vs `:430-614`):
  1. gRPC has **no filename-collision handling** — `overwrite_existing` then
     `remove(src)`: second same-named file destroys the first. HTTP creates
     `name_1/`, `name_2/` subfolders (:171-186).
  2. gRPC **discards DB-write return values** (:732-733) — a failed insert still
     counts `success_count++` and still deletes the source file. HTTP throws and
     rolls back (:227-232).
  3. gRPC skips pre-existing-row cleanup (:216-219 HTTP) and eject-history cleanup
     (:226 HTTP).
- **Change:** create `src/services/eject_service.{hpp,cpp}` with
  `ejectErrored/undoEjectErrored/ejectIgnored/undoEjectIgnored` returning the
  existing `BulkOpResult`; move the **HTTP** implementation in; both surfaces
  marshal only. Repeat the pattern (it already exists — see
  `bulkEject`/`bulkUndoEject` shared via `web_handlers_duplicates.hpp:58-65`, and
  `MediaLocationsServiceImpl` over `FilesService`) for: duplicate-groups listing,
  preview generation (three copies today: `grpc_server_core.cpp:1521-1573`,
  `web_handlers_preview.cpp:151-175` and `:201-215`), server status (three
  implementations), purge, reset, clear-caches. ~900 lines deleted, 3 latent
  data-loss bugs fixed.
- **Tests:** current tests cover the HTTP fork (`test_eject_collision.cpp`,
  `test_purge_ejected_errored.cpp`, `test_duplicates_reset_api.cpp`) — repoint them
  at the extracted services so coverage survives AR-3.

### AR-3. Delete the legacy HTTP surface (~7,300 lines)

> **STATUS: ✅ DONE 2026-08-06 (validated).**
> **-8,841 lines** (net +8/-8841 incl. tests and 1.8 MB of static swagger
> assets). Deleted wholesale:
> - `src/core/webserver/` (10 TUs + `static/`), `include/core/web/`
>   (9 headers), `src/core/server_static_file_handler.cpp`.
> - The `--web` CLI option, `handleWeb`, `start_web_server_`,
>   `initializeWebServer()`, the web branch of the config-change callback
>   (host/port restart), the shutdown handler's web parameter + stop
>   block, and `scripts/run.sh`'s `--web` flag + HTTP endpoint banner.
> - Dead tests of the dead surface: `test_swagger_ui_handler` (15) and
>   `test_web_handlers_preview` (2 — the underlying transcode/cache logic
>   is covered by transcoding-pipeline and preview-cache-ops suites); the
>   WebSocket mock stubs in two other test files; the orphan
>   `simplified_web_server` example. AR-2's repointed eject/purge tests
>   carry the correct-fork coverage forward as designed.
> - The console `restart` command (which restarted only the web server)
>   is now an explicit no-op with a log message; the launcher restarts
>   the whole process.
> - **Security payoff:** the zero-auth destructive endpoints and the
>   factory-default `0.0.0.0` bind are gone with the surface.
> - **Orphans noted for CL-2/AR-6:** `server.host`/`server.port` config
>   keys (and `ServerConfigManager`'s defaults for them) now have no
>   consumer; `web_handlers_config`/`user_settings`/`server_status`/
>   `cache` HTTP counterparts died with no extraction needed (gRPC has
>   its own implementations, per the AR-2 deferral reasoning).
> - **Suite:** 323 tests, 291 passed / 32 pre-existing skips / 0 failed;
>   server binary links and builds clean.
> - **Validate:** `./restart -b`; the app must work exactly as before
>   (it is pure gRPC). `curl http://localhost:8080/` should now connect-
>   refuse. Startup log reads "API: gRPC over /tmp/vurio_dedup.sock".

- **Preconditions:** AR-1, AR-2 done; QW-8's logic lives in the shared service.
- **Evidence it is dead in production:** HTTP starts only with `--web`
  (`src/core/server.cpp:106-108, 183-185`); the SwiftUI launcher passes no args
  (`swift_ui/Sources/swift_ui/SubprocessManager.swift:13-25`); only
  `scripts/run.sh:285,327,602` passes `--web`. It is also a security liability:
  destructive endpoints with **zero auth**, factory-default bind `0.0.0.0`
  (`config_manager_factory.cpp:349`; config.yaml overrides to localhost but a fresh
  install doesn't).
- **Delete:** `src/core/webserver/` (incl. 1.8 MB of `static/` with stale
  swagger/openapi.json), `include/core/web/`, `src/core/server_static_file_handler.cpp`,
  the `--web` option, CMake entries (`CMakeLists.txt:228-238` region), and
  `scripts/run.sh`'s `--web` usage. If a debug HTTP surface is wanted later,
  rebuild as a ~300-line adapter over the services.

### AR-4. gRPC auth via interceptor, not 34 copy-pasted lines

> **STATUS: ✅ DONE 2026-08-06 (validated).**
> Implemented as an **AuthMetadataProcessor**, not a ServerInterceptor —
> a deliberate deviation with a reason: a C++ server interceptor cannot
> cleanly reject a sync RPC with a status before the handler runs
> (TryCancel races the handler and yields CANCELLED, not
> UNAUTHENTICATED), while the processor closes the call with the right
> status BEFORE any handler executes.
> - **`SessionAuthProcessor`** enforces `x-vurio-token` for every method;
>   `/vurio.dedup.SecurityService/Handshake` is exempt via the `:path`
>   pseudo-header. The 34 copy-pasted `requireSession` guards are deleted
>   — and no future RPC can forget the check.
> - **Empirical finding:** `SetAuthMetadataProcessor` on
>   `InsecureServerCredentials` ABORTS with "Not Supported" on gRPC 1.80.
>   Fix: `grpc::experimental::LocalServerCredentials(UDS)` — identical
>   wire format (no TLS, plain h2c) but a real security context, so the
>   processor works. The existing tests' insecure client channels still
>   connect, proving wire compatibility with plaintext clients.
> - **Pairing design unchanged** (0600 cookie file, unix socket 0600,
>   constant-time compares) — it was sound.
> - **ResourceQuota added:** sync worker threads capped at 32 (previously
>   unbounded — a thumbnail-scroll burst could spawn without limit). The
>   CC-6 Shutdown deadline the plan asked for was already in place.
> - **Tests:** `CallWithoutTokenIsRejectedBeforeHandler` (UNAUTHENTICATED,
>   not the handler's UNAVAILABLE — proves pre-handler enforcement),
>   `CallWithWrongTokenIsRejected`, `HandshakeIsExemptFromAuthAndTokenWorks`
>   (full pair→call round trip). Suite: 326 tests, 294 passed / 32
>   pre-existing skips / 0 failed.
> - **Validate (IMPORTANT):** `./restart -b` and confirm the Swift app
>   still connects and loads media — grpc-swift's `.plaintext` speaks the
>   same h2c as the C++ insecure client that passes in tests, but the app
>   is the definitive check. If the app failed to pair/connect, tell me
>   immediately — the credentials change would be the cause.

- **File:** `grpc_server_core.cpp:91-104` (`requireSession`), repeated in 34 of 35
  RPCs; builder at `:1703-1717`.
- **Change:** implement `grpc::experimental::ServerInterceptorFactoryInterface`
  enforcing the session token for every method except
  `/vurio.dedup.SecurityService/Handshake`; register via
  `builder.experimental().SetInterceptorCreators(...)`; delete the per-RPC lines.
  Keep the existing pairing design (cookie file, unix socket, constant-time compare)
  — it is sound. Also add a `grpc::ResourceQuota`/sync-server thread limits, and a
  deadline on `Shutdown()` (CC-6).

### AR-5. Split the gRPC god file and make services testable

> **STATUS: ✅ DONE 2026-08-06 (validated).**
> `grpc_server_core.cpp`: 1,744 (plan-time) → 1,322 (after AR-2/AR-4
> shrank the clones/guards) → **172 lines** — now ONLY server lifecycle
> (secret publication, builder wiring, auth registration, socket perms,
> shutdown).
> - Each service impl moved VERBATIM into its own constructible header
>   under `include/core/grpc_server/`: `security_service.hpp` (35),
>   `config_service.hpp` (58), `media_locations_service.hpp` (115),
>   `duplicates_service.hpp` (350), `advanced_settings_service.hpp`
>   (273), `media_service.hpp` (289). Header-defined by design: verbatim
>   moves carry zero rewrite risk, and only grpc_server_core.cpp + tests
>   include them (via a shared `grpc_service_common.hpp` prelude), so
>   there is no compile-time blast radius. (Deviation from the plan's
>   `.{hpp,cpp}` pairs — out-of-lining ~1,000 lines of method bodies was
>   all risk, no benefit.)
> - `grpc_auth.hpp`: pairing/session state behind an inline accessor
>   (single instance via function-local static) + the AR-4
>   SessionAuthProcessor. `full_res_cache_registry.hpp`: the MP-4 LRU
>   accessor shared by MediaService (fills) and AdvancedSettings
>   (QW-9 clear). The dead file-static `calculateFileSHA1` copy died in
>   the move.
> - **The point, proven:** `ServiceImplsAreDirectlyConstructible`
>   instantiates `MediaServiceImpl` and `ConfigServiceImpl` in-process
>   and calls their handlers as plain methods — exactly what the old
>   test file documented as impossible. The stale apology comment is
>   replaced.
> - **Suite:** 327 tests, 295 passed / 32 pre-existing skips / 0 failed.
> - **Validate:** `./restart -b`; pure refactor — app behavior must be
>   identical.

- **File:** `grpc_server_core.cpp` (1,744 lines, six service classes private to the
  TU — `tests/unit/test_grpc_media_service.cpp:77-81` documents being unable to
  instantiate them).
- **Change:** one file per service impl
  (`src/core/grpc_server/{security,config,media_locations,duplicates,advanced_settings,media}_service.{hpp,cpp}`)
  with constructible headers. After AR-2 these shrink to marshalling.

### AR-6. Config subsystem rightsizing
- **Files:** `config_manager_factory.cpp` (709 lines; 175 hand-written
  `createProperty` calls at :292-582), `config_event_manager.cpp:14-21`,
  `config_type_converter.cpp` (410 lines).
- **Changes:**
  1. **Fix `unsubscribeFromConfigChanges`** — it `clear()`s **every** subscriber in
     the process (comment admits it). Its one caller (`media_processor.cpp:115-123`)
     currently kills config reactivity for DatabaseManager, TPM, DuplicateFinder,
     DiskCache, OnnxSessionManager on media-processor shutdown. Change subscribe to
     return a token (`unordered_map<size_t, Callback>`), unsubscribe by token,
     provide an RAII `ScopedSubscription`; convert the six raw-`this` subscribers.
  2. Move the 175-property schema into `config/config.descriptions.json` (already
     loaded at :299-317) extended with `type` + `default`; the 290-line block
     becomes a ~40-line loop.
  3. Delete dead API: `createWithCustomComponents`, `getDefaultConfig`,
     `validateConfig` (zero call sites); write-only fields `auto_save`,
     `validate_on_load`, `validate_on_save`, `enable_file_watching`; collapse the
     four presets to two.
  4. Restrict `ConfigTypeConverter` to the four types actually used
     (bool/int/string/double — measured from all `createProperty`/
     `getPropertyValue` instantiations).
  5. Replace the `std::cout` config-event subscriber (:691-706) with Poco logging.

### AR-7. Error/logging conventions (opportunistic, alongside other work)
- Adopt a `Result<T>`-style return for the database layer distinguishing
  "not found" from "operation failed" — subsumes the current four conventions
  (bool+log, optional, the ad-hoc `bool* lookup_failed` out-params in
  `thumbnail_cache_ops.hpp:57`/`preview_cache_ops.hpp:37`, exceptions).
- Replace bare `catch (...)` (215 sites, 36 empty) with
  `catch (const std::exception&)` + log except at thread boundaries/destructors.
- Ban `std::cout`/`std::cerr` outside pre-logger `main.cpp` (36 + 16 sites;
  worst: `onnx_adapter.cpp` ×11, `web_server_core.cpp` ×14).
- Make gRPC business failures consistent (13 methods return `OK` with
  `success=false`; MediaService uses real status codes — pick one convention).

---

## Phase 6 — Cleanup / dead code

### CL-1. Video/audio vestiges (photos-only product, commit 96e9aec)
- **The bug:** `config/config.yaml` still sets all 14 `media.video.*` flags **true**
  (:61-165 various), overriding the flipped factory defaults
  (`config_manager_factory.cpp:528-541`). The processing gate
  (`media_processor.cpp:169` → `isFileTypeSupported`) reads the config flag, so an
  existing install can route a video to `VideoProcessor::Process` — a stub that
  returns `true` (`video_processor.cpp:6-12`) → file marked `processed = 2` with
  zero artifacts.
- **Changes:** migrate config.yaml video flags to false; make
  `getFileTypeCategory()` return UNSUPPORTED for `media.video.*` (match the
  hard-coded scanner gate at `media_processor.cpp:1207-1216`); delete
  `video_processor.{cpp,hpp}`, `audio_processor.{cpp,hpp}` (zero references), the
  VIDEO/AUDIO routing branches (`media_processor.cpp:471-770`, unreachable once the
  gate is fixed), their pool-share config, and CMake entries.

### CL-2. Dead code inventory (verified zero production references)
- `src/media_processors/image/image_task.cpp` (whole file; also has the sleep issue
  in CC-6), ~~`features_adapter.cpp`, `OpenCvAdapter::ComputePhash`~~ (✅ deleted
  in DF-5), `TranscodingPipeline::TranscodeToMemory` (tests only),
  `ImageMagickTranscoder::{configureImageProperties,setJpegFormatOptions}`,
  `ThumbnailGenerator::generate` file-variant (prod uses `generateToBlob`),
  `src/utils/stderr_capture.cpp` (unreferenced; would deadlock stderr if used),
  `src/database/session/session_lease.cpp` (broken, zero callers — see DB-4),
  `BackupDatabase`/`GetLatestBackup` RPCs (`grpc_server_core.cpp:1230-1242` —
  return `success=true` and do nothing: implement or remove from the proto),
  `ThumbnailCleanupHandler` `check_source` (TODO returning 0).
- Dead config keys: `thumbnail.generation.timeoutMs` (real timeout comes from
  `media.image.transcoding.timeoutMs`), `thumbnail.allowed.sizes` (real list is
  hard-coded `VALID_SIZES`), `database.sqlite.cacheSize`, `database.sqlite.tempStore`
  (QW-7), ~~`duplicates.metadata.*`~~ (✅ removed in DF-5).
  Wire or delete each.
- Working-tree litter: `src/filesmanager/disk_cache.cpp.bak`; root-level empty DBs
  (`database.db`, `database.sqlite`, `media_dedup.db`, `storage.db` — all 0 bytes,
  referenced by no code; the real store is `data/dedup_server.db`); the hundreds of
  `test_gif*.jpg` files at `dedup_server/` root; stale `report_dynamo_export.sarif`,
  `server_stdout.log` (3.9 MB).
- Naming: `MediaDedupServer` is both a class (`MediaDedup::MediaDedupServer`) and a
  namespace (`MediaDedupServer::Core`), forcing `::`-qualified escapes; two `Core`
  namespaces exist. Collapse when convenient (mechanical, wide diff — do late).
- Unbounded maps to cap: `MediaGenerationFailures` (pruned only on success),
  thumbnail `generation_locks_` (CC-6).

---

## Suggested session-sized batches

| Session | Tasks | Outcome |
|---|---|---|
| 1 | QW-1 … QW-8 | One PR of one-liners; cache 10× smaller, no restart re-transcode, pause works, write loss stopped |
| 2 | DB-1, DB-2 | Day view ms-fast; DB half the size; pool contention gone |
| 3 | DB-3, DB-4, DB-5 | Pool churn + acquire path + injection fixed |
| 4 | DB-6, DB-7 | Indices/ANALYZE; transactions & N+1s |
| 5 | CC-1 … CC-3 | Deadlocks and lost-work bugs |
| 6 | CC-4 … CC-6 | Scheduler/lifecycle/shutdown safety |
| 7 | DF-1, DF-2 | Dedup: load-once + real ANN (hours → minutes) |
| 8 | DF-3 | Union-find + edge list; sub-second slider |
| 9 | DF-4 (+DF-5 decision) | Embedding throughput 5–20× |
| 10 | MP-1 … MP-5 | Ingest halved; LRU cache; payload slimming |
| 11 | AR-1, AR-2 | Upward edges broken; shared services; data-loss clones killed |
| 12 | AR-3, AR-4 | HTTP layer deleted (−7,300 lines); auth interceptor |
| 13 | AR-5, AR-6 | gRPC split + testable; config rightsized |
| 14 | CL-1, CL-2, AR-7 | Dead code purge; conventions |

## What to preserve (do not "fix")

- The acyclic layering `config → database → filesmanager → orchestration → core`
  and constructor injection throughout (only 3 true singletons).
- `SessionManager::Lease` RAII discipline (133 call sites, no leaks — the problems
  are hold-duration and churn, not the pattern).
- The gRPC media transport: **paths over a unix socket, never image bytes** —
  streaming image chunks would be a regression.
- `bulkEject`/`bulkUndoEject` — the reference implementation for transaction +
  filesystem ordering, and the template for AR-2.
- LibRaw embedded-thumbnail extraction (`raw_embedded_extractor.cpp`) — correct
  fast path; don't replace with demosaic.
- The async "UNAVAILABLE + SubscribeToMediaReady push" generation pattern — the
  Swift client's 60 s deadline + push wakeup matches it.
