# DISC-1: replace the id watermark with per-file discovery state

> **Status: DONE, 2026-08-15.** `scanned_files.discovery_done` replaces the watermark;
> `duplicate_discovery_requeue`, `trg_scanned_files_requeue_late_processed`,
> `processRequeue`, `requeueForDiscovery` and the `last_processed_id` selector are gone.
> See §10 for what shipped and how it differs from the plan in §6.
>
> **Why it was still worth doing after the §5 stopgap.** The stopgap narrowed the hole
> but did not close it. The trigger tested `NEW.id <= MAX(last_processed_id)` against the
> *persisted* checkpoint, and the checkpoint is only written after a batch commits
> (`processBatch`, after `discoverEdgesForIds` returns) while `record_progress` advanced
> the in-memory watermark to the batch's top id. So for the whole duration of a batch
> (~97 s per 1,000 files), a file reaching `processed = 2` with an id inside the batch's
> range was neither in the batch's snapshot nor caught by the trigger — and once it sits
> at 2, the trigger can never fire for it again, because it requires
> `OLD.processed <> 2`. `mediaProcessor` (30 s) and `duplicateFinder` (1 h) are separate
> scheduler jobs and the processor's pool completes out of id order, so the stragglers
> below the batch's top id are exactly the population at risk, on every batch of an
> import. `FileFinishingDuringADiscoveryBatchIsNotLost` is the regression test for it.
>
> **Origin:** deferred as item 5 of DF-3 in
> [backend_optimization_plan.md](backend_optimization_plan.md) ("the checkpoint stays a
> single id watermark; re-embedded files below it are still never re-compared. Revisit
> with an `artifact_updated_at` cursor or dirty flag **if/when re-embedding becomes a
> real flow**"). That deferral rested on a premise the measurements in §3 disprove: the
> hole is not confined to re-embedding. Ordinary first-time processing walks into it,
> and it had silently damaged 13.4% of duplicate groups.
>
> **Line numbers** are as of the DF-5 working tree on top of `43228462`. Re-locate by
> the quoted identifiers if the files have drifted.

---

## 1. Summary

Edge discovery decides "which files still need to be compared" using a single integer,
`duplicate_processing_checkpoint.last_processed_id`. That is only correct if files reach
`processed = 2` in ascending id order. They do not — status is written per `file_path`
by a parallel worker pool, and can be reset and re-set long afterwards. Every file that
finishes behind the watermark is skipped permanently: it never runs a KNN query of its
own, so it only ever collects edges from files discovered *after* it. Two such files
never see each other, and their pair is lost forever.

The fix is to stop inferring discovery state from an id ordering and store it per file.

---

## 2. How discovery worked, and why the watermark was wrong

*(§2–§7 are the original analysis and plan, kept as the record of why. §10 is what landed.)*

Batch selection — `duplicate_finder.cpp:510`, inside `processBatch` (`:495`):

```sql
SELECT id FROM scanned_files WHERE id > ? AND processed = 2 ORDER BY id ASC LIMIT ?
```

The watermark then advances to the highest id in the batch (`record_progress`,
`duplicate_finder.cpp:599`), using a max-CAS so parallel fan-out cannot move it
backwards. Both halves are individually reasonable; together they assume `processed = 2`
arrives in id order.

It does not, for at least four reasons:

| Path | Effect |
|---|---|
| `scanned_files_ops.cpp:675` / `kUpdateProcessed` | status written per `file_path` by a parallel worker pool — completion order is not id order |
| `sql_constants.hpp:279` | `processed=1 → 0`, requeuing in-flight work after a restart |
| `sql_constants.hpp:284` | bulk error reset (`processed < 0 → 0`), re-running files that failed |
| `eject_service.cpp:810` | drive restore rewrites `processed` for every path on the volume |

Any file whose status flips to 2 after a higher id advanced the watermark is never
selected again. Nothing logs it. The `duplicate_edges` row simply never exists.

Note the asymmetry that makes this so hard to see: such a file still *looks* grouped.
Later files run their own KNN, find it in the vector index, and write edges to it. It
accumulates a plausible-looking edge list — just one that is missing precisely its
nearest neighbours, because those are the files that were being processed alongside it.

### Why DF-4's requeue does not cover it

The `duplicate_discovery_requeue` table (DF-4) parks files owed a second look, drained by
`processRequeue` (`duplicate_finder.cpp:752`) with `watermark = nullptr` so the pass is
symmetric. But it is only fed from *inside* a discovery batch (no embedding at discovery
time), from `VectorIndex::reconcile()`, and from index-insert failure. A file that was
never selected for a batch at all is invisible to every one of those.

---

## 3. Measured impact

Live library `data/dedup_server.db`, measured 2026-08-14 on a **copy** (server untouched):

```
scanned_files                          143,124
processed = 2                          141,990
artifacts with embedding               140,985
vec_embedding_index rows               140,985   (index itself complete — 0 missing)
duplicate_edges                     12,075,106
duplicate groups                        42,148   (123,383 grouped files)
```

Detection heuristic: a file that ran its own KNN pass almost always ends up with at least
one *lower*-id peer. Files with none never ran one.

```
embedded + processed files             140,985
  zero edges at all                      1,395   ( 0.99%)
  edges, but ALL peers higher-id         11,316   ( 8.03%)
  => never ran their own pass            12,711   ( 9.02%)

groups containing >=1 such file          5,628 / 42,148  (13.4%)
grouped members among them               6,568
```

Effect on the DF-4 join guard, sampling 400 non-representative members and recomputing
`best_neighbour` from a live k=200 KNN instead of from `duplicate_edges`:

```
vetoed using the edge table (what happens)      0   (0.0%)
vetoed using true nearest  (what should)       11   (2.8%)
```

Every miss in that sample was a hidden edge, not a bad ratio — several were files sitting
in a group at ~0.94 while a **1.0000** twin went unlinked. Extrapolated across ~81k
non-representative members that is on the order of 2,000 wrong memberships.

The guard is not at fault. It reads `best_neighbour` from `duplicate_edges`
(`duplicate_finder.cpp:1323-1337`), so missing edges make it under-trigger silently.

---

## 4. Worked example — group 27423

The group the defect was reported from. Two unrelated scenes in one group:

| id | file | stored score → rep |
|---|---|---|
| 85677 | `2019-09-21/DSC05091.ARW` | 1.0000 (representative) |
| 89628 | `2019-08-26/DSC05091.ARW` | 1.0000 |
| 73123 | `2023-05-29/_DSF6260.JPG` | 0.9119 |
| 73134 | `2023-05-29/_DSF6260.RAF` | 0.8886 |

True cosines from the vector index:

```
73123 x 73134 = 0.9829     <- the JPG/RAF pair. NO EDGE IN duplicate_edges.
73123 best    = 0.9863  (_DSF6258.JPG)
73134 best    = 0.9875  (_DSF6256.RAF)
```

Both Fuji files have `min_peer` far above their own id (73123's lowest peer is 75696) —
neither ran its own pass, so the edge between them was never written from either side.
The guard therefore saw a best alternative of 0.9300 (an unrelated 2017 JPG that *had*
run a pass and found them) and admitted both, at `k = 2.0`:

```
73123:  (1-0.9119)=0.0881  <=  2 x (1-0.9300)=0.1401   admitted
73134:  (1-0.8886)=0.1114  <=  2 x (1-0.9247)=0.1505   admitted
```

With the real neighbours restored, both are vetoed and leave:

```
73123:  0.0881  >  2 x (1-0.9863)=0.0274   VETO
73134:  0.1114  >  2 x (1-0.9875)=0.0251   VETO
```

---

## 5. The stopgap this task removed

Two changes landed 2026-08-14. They work, and the repair is a genuine one-off, but both
exist only because discovery state is inferred rather than stored.

**a) `trg_scanned_files_requeue_late_processed`** — `sql_constants.hpp:405-418`, created
in `DuplicateGroupsOps::ensureTables` (`duplicate_groups_ops.cpp:96-97`):

```sql
AFTER UPDATE OF processed ON scanned_files
WHEN NEW.processed = 2 AND OLD.processed <> 2
     AND NEW.id <= COALESCE((SELECT MAX(last_processed_id)
                             FROM duplicate_processing_checkpoint), 0)
  -> INSERT OR IGNORE INTO duplicate_discovery_requeue(file_id, reason)
     VALUES (NEW.id, 'processed_below_watermark');
```

A trigger was chosen so every writer is covered without touching four call sites.
`scanned_files.id` is `AUTOINCREMENT`, so fresh inserts are always ahead of the watermark
and no INSERT trigger is needed. Side effect worth knowing: `ensureTables` now also
ensures `scanned_files` exists, because the trigger hangs off it and the previous
init ordering (`FilesManager` before `DuplicateFinder`) was implicit.

**b) `kEdgeDiscoveryEpoch = 2`** — `duplicate_finder.cpp:67`. One-time per database: on
startup a DB stamped with a lower epoch has every embedded file requeued for a symmetric
pass, then `duplicate_edges_meta.discovery_epoch` is stamped and it never fires again. It
does **not** wipe edges, groups, or the watermark. Cost on this library: 140,985 files at
~1,000 per 97 s (k=200 exact KNN measured at 104 ms/query) ≈ **3.8 hours** of background
work.

### Cost of keeping the stopgap

- A whole table, a trigger, and a version constant to express what one column would.
- The requeue never empties. It currently holds **1,005** `no_embedding_at_discovery`
  rows, oldest parked **2026-08-11**. None are videos; all 1,005 still have no
  `image_artifacts` row — they are images whose processing failed. The drain re-JOINs
  them on every run, forever. The DF-4 comment calls the join "the whole retry policy",
  which is deliberate, but it means a permanent backlog with no expiry.
- Correctness now depends on a SQLite trigger staying in sync with the C++ selector. If
  the selector changes and the trigger does not, the hole reopens silently — which is
  exactly the failure mode this whole task is about.

---

## 6. Proposed change (see §10 for the four corrections made while building it)

Store discovery state per file instead of inferring it from an id ordering.

```sql
ALTER TABLE scanned_files ADD COLUMN discovery_done INTEGER NOT NULL DEFAULT 0;
CREATE INDEX idx_scanned_files_pending_discovery
    ON scanned_files(discovery_done) WHERE processed = 2 AND discovery_done = 0;
```

Selector becomes, with no ordering assumption and no watermark:

```sql
SELECT sf.id FROM scanned_files sf
JOIN image_artifacts ia ON ia.file_path = sf.file_path
WHERE sf.processed = 2 AND sf.discovery_done = 0 AND ia.embedding IS NOT NULL
LIMIT ?
```

`discovery_done` is set to 1 after a file's pass commits. Everything the requeue exists
for collapses into clearing the flag:

| today | after |
|---|---|
| requeue row `no_embedding_at_discovery` | flag stays 0; the `ia.embedding` predicate holds the file back until it has a vector — no row, no permanent backlog |
| requeue row `vector_index_backfill` (`VectorIndex::reconcile`) | `UPDATE scanned_files SET discovery_done = 0 WHERE id IN (...)` |
| requeue row `index_insert_failed` | same |
| requeue row `processed_below_watermark` (trigger) | **nothing** — a late `processed = 2` is just a row the selector has not reached yet |
| `kEdgeDiscoveryEpoch` bump + seeding | `UPDATE scanned_files SET discovery_done = 0` |

Deletions: `duplicate_discovery_requeue`, `processRequeue`, the trigger,
`kEdgeDiscoveryEpoch`, and the `last_processed_id` watermark plumbing
(`discoverEdgesForIds`' `watermark` parameter, `record_progress`, `peer_dedup_watermark`
at `:597`). Net a smaller surface than what exists now.

This also finally closes DF-3 item 5 as originally stated: a re-embedded file just gets
its flag cleared.

### Points to decide

1. **Column vs. side table.** A column on `scanned_files` is simplest but writes to a hot
   143k-row table on every discovery. A `pending_discovery(file_id)` side table keeps the
   write off `scanned_files` and is closer to what exists — but reintroduces a table
   whose rows can be orphaned, which is half of what we are trying to delete. Recommend
   the column; measure the write cost before committing.
2. **Non-KNN fallback path.** `discoverEdgesForIds` still has a pool-comparison branch
   used when the vector index is unavailable, and `peer_dedup_watermark` (`:597`) is what
   stops it recording each pair twice. Dropping the watermark needs that branch to
   de-duplicate another way (record from the lower-id side only, which is already the
   storage convention).
3. **Batch abort.** `processBatch` deliberately does not advance the checkpoint on abort
   (`:524`). With a flag, per-file state means a partial batch keeps whatever it finished
   — strictly better, but confirm nothing downstream assumes all-or-nothing.
4. **Ordering.** The current selector is `ORDER BY id ASC`; a flag-driven one has no
   inherent order. Grouping is already order-independent by construction (DF-3), so this
   should be free — worth an explicit test.

---

## 7. Migration (as shipped; verified numbers in §10)

One-time, on the first startup after the change:

```sql
-- everything the old watermark legitimately covered is already discovered
UPDATE scanned_files SET discovery_done = 1
 WHERE processed = 2
   AND id <= (SELECT COALESCE(MAX(last_processed_id), 0)
              FROM duplicate_processing_checkpoint);
```

Everything else stays 0 and is picked up naturally. Any rows still sitting in
`duplicate_discovery_requeue` must be folded in (`discovery_done = 0`) **before** the
table is dropped. If this lands after the epoch-2 repair has completed, the flag can be
set to 1 for every embedded file and the migration is effectively free; if it lands
mid-repair, the safe move is to leave requeued ids at 0.

Keep `duplicate_edges_meta.floor` — the floor-provenance reset is a separate mechanism
and still needed.

---

## 8. Verification (see §10 for what is covered and what is still owed)

- Unit: port `FileProcessedBelowTheWatermarkIsRequeuedAndRepaired` and
  `FileProcessedAheadOfTheWatermarkIsNotRequeued`
  (`tests/unit/test_duplicate_finder_range.cpp`) to the flag — same scenarios, and they
  must pass without a trigger. Keep `RequeueRestoresEdgesTheWatermarkCannotReach` in
  spirit (backfilled vector → flag cleared → edges restored).
- Add: error reset (`processed < 0 → 0 → 2`) and drive restore must both leave the file
  discoverable, with no trigger involved.
- Library-scale: re-run the §3 heuristic. `edges, but ALL peers higher-id` should fall to
  ~0 on a fully drained library; anything left is a genuine finding.
- Regression guard: the §3 400-member sample should show edge-table vetoes and
  true-nearest vetoes **agreeing**. They disagreed 0 vs 11 before this work.

---

## 9. Naming collision to clean up while here — DONE, see §10

`DF-N` means two different things. In
[backend_optimization_plan.md](backend_optimization_plan.md): DF-4 = embedding throughput
(ONNX), DF-5 = optional pre-filter. In `duplicate_finder.cpp` comments: DF-4 = join guard
+ requeue, DF-5 = this watermark work. DF-3 happens to agree in both. The code comments
were written against the wrong scheme; renumber them (or drop the prefix) when this task
lands, so the doc IDs stay the single authority.

---

## 10. What shipped (2026-08-15)

Implemented as §6, with four corrections found while building it.

### Schema

`discovery_done INTEGER NOT NULL DEFAULT 0` on `scanned_files`, declared in
`kCreateScannedFilesTable` for fresh installs and applied by
`kAlterScannedFilesAddDiscoveryDone` for existing ones. It is the LAST column in both,
because `ADD COLUMN` appends and the two definitions have to stay structurally
identical (same rule as `taken_day` / `is_video`). The partial index is
`ON scanned_files(id) WHERE processed = 2 AND discovery_done = 0` — indexed on `id`,
not on `discovery_done`, which §6 had wrong: the column is a constant inside the
predicate. Same shape as the existing `idx_scanned_files_unprocessed`.

### Corrections to §6

1. **The fallback path's de-duplication rule in §6 point 2 was wrong.** "Record from the
   lower-id side only" loses edges: a pending file `X` compared against an
   already-discovered pool peer `Y < X` would record nothing, and `Y` never runs again.
   The peer's *batch membership* — not its id — is the exact answer to "will the other
   side look too", so the skip is now
   `batch_set.count(other.file_id) && other.file_id > current_file_id`.

2. **A flag-only selector spins forever.** `processBatch` loops until a batch comes back
   empty; a row that is selected but whose artifact fails to load keeps its flag at 0
   and is re-selected immediately. Fixed with an in-memory, **per-run** cursor
   (`id > cursor ORDER BY id ASC`), reset to 0 at the start of every run and never
   persisted. Correctness lives entirely in the flag; the cursor only stops one run from
   re-attempting what it already tried, so anything it skips is picked up by the next
   run. This also settles §6 point 4 — ordering stays `id ASC`.

3. **`resetCheckpoint` is the "start discovery over" primitive** (`ResetDuplicates` in
   the advanced-settings API, and the floor-provenance reset in
   `DuplicateFinder::initialize`). It now clears `discovery_done` for every file inside
   the same call. Without that, zeroing the checkpoint would reset nothing and the next
   run would find no work at all.

4. **§6 missed a consumer:** `ScannedFilesOps::getStatusCounts` fed the UI's dedup
   progress bar from `SUM(processed = 2 AND id <= last_processed_id)`. It is now
   `SUM(processed = 2 AND discovery_done = 1)` (and `countProcessedUpTo` became
   `countDiscovered`, losing its `max_id` parameter). This makes the bar strictly more
   honest: it used to count files the watermark had passed but that had never run a
   pass.

### Also changed

- **`kEdgeDiscoveryEpoch` kept, not deleted** (§6's deletion list and its own mapping
  table disagreed). It is still the versioning hook for future discovery-correctness
  fixes; its body is now `UPDATE scanned_files SET discovery_done = 0`. Bumped to
  **3** to sweep up whatever the §5 race window leaked after the epoch-2 repair.
- **Re-embedding clears the flag.** `ImageArtifactsOps::upsertEmbedding` clears
  `discovery_done` in the same lease as the write, so a rewritten vector is compared
  again instead of keeping edges from the old one. This is what finally closes DF-3
  item 5 as originally stated; nothing did it before.
- **`VectorIndex::requeueForDiscovery` → `clearDiscoveryDone`**, and it no longer has to
  create a table on its own connection first.
- **Batch abort semantics unchanged** (§6 point 3): an aborted batch writes neither
  edges nor flags, so it stays entirely pending. Keeping partial progress would need
  restructuring for no real gain.
- **Epoch sweeps still drive `repairProgress()`** so the UI's repair bar keeps working;
  ordinary new-file discovery does not, which matches the old requeue-drain semantics.
  An aborted sweep re-arms, so an interrupted repair still reads as one.
- **§9 done:** the mis-numbered `DF-4` / `DF-5` comment prefixes in the duplicate-finder
  files were dropped (the ONNX-side `DF-4` tags are correct per
  `backend_optimization_plan.md` and were left alone).

### Migration, verified against the live 143k-file library

The one-time migration is guarded by the presence of `duplicate_discovery_requeue`, so
it runs exactly once and never on a fresh database, and it folds requeued ids back to 0
before the table is dropped. Run against a copy of `data/dedup_server.db`:

```
migrated to discovery_done = 1        140,985
pending and workable                        0
pending, no artifact row                1,005   (the ex-permanent requeue backlog)
```

The 1,005 are files marked `processed = 2` whose processing never produced an
`image_artifacts` row. Under the requeue they were re-joined on every run forever;
now they are simply rows the selector's `ia.embedding IS NOT NULL` predicate does not
reach, at zero cost. (That they are `processed = 2` with no artifact at all is a
separate defect in the processing pipeline, not addressed here.)

### Tests (all in `tests/unit/test_duplicate_finder_range.cpp`, in the build)

| test | what it pins |
|---|---|
| `FileFinishingDuringADiscoveryBatchIsNotLost` | **the race the §5 trigger could not see** — reproduces the exact end state (processed, embedded, id behind the watermark, nothing queued) |
| `FileProcessedAfterDiscoveryPassedItIsStillDiscovered` | §8's ported watermark scenario, passing with no trigger |
| `NewlyProcessedFileIsDiscoveredWithoutAnyQueue` | the normal path needs no special handling, and records each pair once |
| `ClearingTheFlagRestoresEdgesTheWatermarkCouldNotReach` | `reconcile()`'s backfill path; one side reopened restores the pair |
| `ProcessedFileWithoutEmbeddingIsHeldBackNotDropped` | held back, not parked and not dropped |
| `ErrorResetLeavesTheFileDiscoverable` | `processed < 0 → 0 → 2`, no trigger involved |
| `DriveRestoreLeavesTheFileDiscoverable` | eject/undo-eject rewrite of `processed` |
| `ReEmbeddingReopensAFileForDiscovery` | DF-3 item 5 |
| `ResetCheckpointReopensEveryFileForDiscovery` | correction 3 |
| `MigrationOffTheIdWatermarkPreservesWhatWasOwed` | discovered vs. owed vs. never-reached, teardown, idempotence |
| `FreshDatabaseStartsWithNothingDiscovered` | no migration and no stopgap artifacts on a new database |
| `EdgeDiscoveryEpochBumpReopensEveryFileWithoutWiping` | the sweep is additive |
| `EmptyEdgeTableWithDiscoveredFilesReopensDiscovery` | "has discovery run" read off the flag |

Full unit suite: 308 passed, 32 skipped (pre-existing ONNX/ImageMagick/MediaProcessor
environment skips), 0 failed.

### Verified on the live library (2026-08-16)

The epoch-3 sweep ran on `data/dedup_server.db`: discovery state cleared on 143,124 rows
at 21:16:47 UTC, first batch 22:17:47, `processed 140985 files in 141 batches` at
02:03:31 — **3 h 45 m 44 s**, against the 3.8 h estimate.

Re-running the §3 heuristic before and after that sweep gives identical numbers:

| | before sweep | after sweep |
|---|---|---|
| `duplicate_edges` | 14,939,766 | **14,939,766** |
| zero edges at all | 1,148 | 1,148 |
| edges, but ALL peers higher-id | 3,772 | 3,772 |

**A full symmetric k=200 pass over every embedded file produced zero new edges.** Two
conclusions:

1. The edge list is closed under the current index and floor — there is nothing left for
   discovery to find on this library.
2. It settles §8's open question about the heuristic's residual. Those 3,772 files are
   not hiding missing edges: each one just re-ran its own pass and nothing appeared, so
   they are edge-component minima by construction. The 3.5% residual after the epoch-2
   repair was entirely heuristic false-positives, not damage.

That also collapses §8's second check. The join guard reads `best_neighbour` from
`duplicate_edges`, and the edge table is now provably equal to what a fresh KNN pass
produces, so edge-table vetoes and true-nearest vetoes agree by construction — the 0 vs
11 disagreement cannot recur. This holds within `knnK = 200` and at or above the 0.80
edge floor, and assumes the vector index is complete (measured complete at 140,985 rows
on 2026-08-14; not re-checked here, as vec0 is not loadable from the sqlite3 CLI).

**What this does NOT verify.** The sweep found nothing because this library has had no
ingest since the epoch-2 repair, so the batch-window race had nothing to leak. It
confirms the library is consistent; it does not exercise the race fix. The evidence for
that remains `FileFinishingDuringADiscoveryBatchIsNotLost`. The first real test is the
next bulk import.

### Fixed: the repair indicator now survives a restart

`epoch_repair_pending_` was an in-memory atomic, and the epoch is stamped at
`initialize()` — before the sweep it triggers has run a single batch. So a restart in
between lost it, and nothing re-armed it, because by then `stored_epoch` already equalled
`kEdgeDiscoveryEpoch` and the branch could not fire again.

This was not hypothetical. On the first real epoch-3 run the log shows the server started
at 21:16:47 (epoch stamped, 143,124 flags cleared), restarted at 22:16:55 and 22:17:16,
and only then began sweeping at 22:17:47. The entire 3 h 45 m pass ran with
`repairProgress()` at `{0, 0}` and the repair bar dark.

The marker now lives in `duplicate_edges_meta.discovery_repair_pending`, written in the
same startup that clears the flags (after the epoch upsert, so the row is guaranteed to
exist) and cleared only when a run actually drains the pending set. An aborted run leaves
it set, so an interrupted sweep still reads as an episode on the next run — in this
process or a later one. `repairPending()` returns false on any error: an unknown state
should hide the indicator, never strand it.

Pinned by `RepairEpisodeSurvivesARestartMidSweep`, which seeds the epoch in one
`DuplicateFinder`, destroys it before any batch runs, and asserts a second instance still
sees the episode. Mutation-checked: forcing `repair_seeded = false` fails the test on
exactly that assertion.

The main dedup progress was never affected — `duplicate_finding_completed`/`total` is
flag-based and persisted.

### Not a defect: the scheduler interval

An earlier reading of the hour between the epoch stamp and the first sweep batch was
attributed to something overriding `duplicates.finder.intervalMs`. That was wrong. Every
`Registering job: duplicateFinder` line in the log, back to 2026-08-11, reads
`interval: 5000 ms`, matching `config/config.yaml`. The hour was three separate server
starts, not one long wait.
