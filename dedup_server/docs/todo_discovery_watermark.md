# TODO — DISC-1: replace the id watermark with per-file discovery state

> **Status:** open. Not started. A stopgap shipped 2026-08-14 (see §5) that makes the
> symptom go away; this task removes the cause and deletes the stopgap.
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

## 2. How discovery works today, and why the watermark is wrong

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

## 5. What already shipped (the stopgap this task removes)

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

## 6. Proposed change

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

## 7. Migration

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

## 8. Verification

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

## 9. Naming collision to clean up while here

`DF-N` means two different things. In
[backend_optimization_plan.md](backend_optimization_plan.md): DF-4 = embedding throughput
(ONNX), DF-5 = optional pre-filter. In `duplicate_finder.cpp` comments: DF-4 = join guard
+ requeue, DF-5 = this watermark work. DF-3 happens to agree in both. The code comments
were written against the wrong scheme; renumber them (or drop the prefix) when this task
lands, so the doc IDs stay the single authority.
