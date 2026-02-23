# Analysis: Delete Empty Group After Split and Re-queue Key Image

## Context

After **dynamic group splitting**, we remove the split member and redistribute "similar" members from the original group to the new group. The original group’s member count becomes:

`orig_count = 1 + (similar_members.size() - 1 - num_redistributed)`

(all members except the rep are similar; we subtract the split member and each moved member).

So we can end up with **only the key image** left in the original group (`orig_count == 1`). A single-image group is not a real duplicate set, so we should:

1. **Delete the original group** (so the key is no longer in any group).
2. **Re-queue the key image** so it is treated again as a candidate and can match other groups or form new ones.

This document analyses how to implement that.

---

## 1. When to Detect “Only Key Left”

**Where:** In `trySplitAndRedistribute` in `duplicate_finder.cpp`, after the redistribution loop, we have:

- `orig_count` = number of members left in the original group (including the representative).
- If `orig_count == 1`, the only remaining member is the representative (key image).

**Action:** Before or after `updateGroupMemberCount(db_, best_group_id, orig_count)`, branch on `orig_count == 1` and then delete the group and re-queue the key.

**Edge case:** The original group might have had only key + one similar member (the split member). After removing the split member, `orig_count` is already 1. So we always need to check `orig_count == 1` after redistribution.

---

## 2. Deleting the Original Group

**API:** `DuplicateGroupsOps::deleteGroup(db_, best_group_id)`.

**Schema:** `duplicate_members` has `FOREIGN KEY (group_id) REFERENCES duplicate_groups(id) ON DELETE CASCADE`. Deleting the group row therefore removes all member rows for that group. The key image’s `file_id` will no longer appear in `duplicate_members`.

**In-memory cache:** We must drop the original group from `group_representatives` so the main loop does not keep comparing against a deleted group. So after delete:

- `group_representatives.erase(best_group_id);`

**Order of operations:**

1. Record `key_file_id = group.representative_file_id` (and path if needed for re-queue).
2. Call `DuplicateGroupsOps::deleteGroup(db_, best_group_id)`.
3. Remove from cache: `group_representatives.erase(best_group_id)`.

We should **not** call `updateGroupMemberCount(best_group_id, orig_count)` when we are about to delete the group; deleting is enough.

---

## 3. What “Re-queue for Duplicate Evaluation” Means

The duplicate finder decides which files to evaluate using:

- **New files:** `scanned_files` with `id > last_processed_id` and `processed = 2`, in batches. The checkpoint advances so each file is only considered once in the “new files” sense.
- **Ungrouped pool:** Files with `id <= existing_last_id`, `processed = 2`, and not in `duplicate_members`. Used for cross-batch grouping when a new file doesn’t match any group rep.

So:

- The key’s `file_id` is usually **≤ last_processed_id** (it was processed in an earlier batch and became a group rep). So it will **not** be selected again by the “new files” query.
- After we delete the group, the key is **no longer in duplicate_members**, so it **does** appear in the ungrouped pool and can be used in **tryCrossBatchGrouping**: new files (id > last_processed_id) are compared against ungrouped files, so the key can form a new group with a later file that matches it.

That gives **partial** re-evaluation: the key can join new groups via cross-batch, but we **do not** re-run it through the full “compare to all group representatives” pass. So we never re-check whether the key fits better in some **existing** group.

To get **full** re-evaluation (key compared again to all reps and ungrouped, like any other candidate), we need one of the mechanisms below.

---

## 4. Options for Re-queuing the Key

### Option A: Delete Only; Rely on Ungrouped + Cross-Batch (No Checkpoint Change)

**Behaviour:**

- Delete the group; key leaves `duplicate_members`.
- Key is in the ungrouped set (`id <= last_processed_id`, `processed = 2`, not in `duplicate_members`).
- It is **not** re-run as a “new file” (id not > last_processed_id).
- It **can** form new groups when a **new** file (id > last_processed_id) matches it in `tryCrossBatchGrouping`.

**Pros:** No schema or checkpoint changes; minimal code (delete group + erase from cache).  
**Cons:** Key is never compared again to existing group representatives; only “re-evaluated” when new files arrive and match it.

---

### Option B: Lower Checkpoint So Key Is Treated as New Again

**Behaviour:**

- Delete the group.
- Set `last_processed_id = min(current_checkpoint.last_processed_id, key_file_id - 1)` and persist it (e.g. via a new helper like `setCheckpointLastProcessedId(mode, key_file_id - 1)` or by reading checkpoint, updating, and upserting).
- Next batch run: “new files” query uses `id > last_processed_id`, so the key (and every file with id ≥ key_file_id) is considered again. The key gets a full pass (compare to all reps, possibly add to group or go to ungrouped / cross-batch).

**Pros:** Full re-evaluation for the key; no new tables.  
**Cons:** Re-processes **all** files with `id >= key_file_id` every time we empty a group, which can be expensive and duplicate a lot of work.

---

### Option C: Dedicated Re-check Queue (Recommended for Full Re-evaluation)

**Behaviour:**

- Add a small table, e.g. `duplicate_recheck_queue (mode TEXT, file_id INTEGER, added_at TIMESTAMP)`, with a unique constraint on `(mode, file_id)` so we don’t queue the same file twice.
- When we detect `orig_count == 1` after a split:
  - Delete the group and erase it from `group_representatives`.
  - Insert `(mode, key_file_id)` into `duplicate_recheck_queue` (if not already present).
- In **processBatch**, before or merged with the “new files” query:
  - Select `file_id` from `duplicate_recheck_queue` for this mode.
  - For each such `file_id`, load artifacts and run the **same** logic as for a new file: skip if already in a group, else compare to all group representatives (best group, duplicate vs similar tier, split analysis, add or cross-batch). After processing a recheck file, **delete** it from `duplicate_recheck_queue`.
- So the key is re-evaluated exactly once per “empty group” event, with full semantics (reps + ungrouped), without moving the main checkpoint.

**Pros:** Full re-evaluation; no re-processing of unrelated files; clear semantics; survives restarts if table is persisted.  
**Cons:** New table and a few new code paths (insert on empty-group, and merging recheck file_ids into the batch or a separate loop).

---

### Option D: Hybrid (Delete + Recheck Queue Only When Useful)

Same as Option C, but we could **skip** inserting into the recheck queue if we know the key will be re-evaluated soon anyway (e.g. if the batch has many new files and cross-batch will consider the key). That adds heuristics and complexity; starting with “always re-queue when we delete an empty group” (Option C) is simpler and predictable.

---

## 5. Recommended Direction

- **Short term / minimal change:** Implement **Option A**: after split, if `orig_count == 1`, delete the group and erase it from `group_representatives`. No re-check queue; the key is only re-used via cross-batch when new files arrive. Easy to ship and correct for “delete empty group”.
- **Full re-evaluation:** Implement **Option C**: add `duplicate_recheck_queue`, and in the “only key left” path delete the group, erase from cache, and insert the key’s `file_id` into the queue; in `processBatch`, process queued file_ids with the same logic as new files and remove them from the queue. That gives true “mark key for re-evaluation like any other candidate” with one full pass and no checkpoint rollback.

---

## 6. Implementation Sketch (Option C)

**Schema (e.g. in `sql_constants.hpp`):**

```sql
CREATE TABLE IF NOT EXISTS duplicate_recheck_queue (
    mode TEXT NOT NULL,
    file_id INTEGER NOT NULL,
    added_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (mode, file_id),
    FOREIGN KEY (file_id) REFERENCES scanned_files(id) ON DELETE CASCADE
);
```

**DuplicateGroupsOps (or similar):**

- `enqueueDuplicateRecheck(db, mode, file_id)` — insert or ignore.
- `dequeueDuplicateRecheckBatch(db, mode, limit)` — return list of file_ids and delete those rows (or delete after successful processing).
- Ensure tables created in `ensureTables` for duplicate detection.

**DuplicateFinder::trySplitAndRedistribute:**

- After redistribution loop, if `orig_count == 1`:
  - `key_file_id = group.representative_file_id`
  - `DuplicateGroupsOps::deleteGroup(db_, best_group_id)`
  - `group_representatives.erase(best_group_id)`
  - `DuplicateGroupsOps::enqueueDuplicateRecheck(db_, mode, key_file_id)`
  - Return true (no need to update member count for a deleted group).
- Else: `updateGroupMemberCount(best_group_id, orig_count)` and continue as today.

**DuplicateFinder::processBatch:**

- At the start, after loading group representatives and ungrouped files:
  - Fetch recheck file_ids: `recheck_ids = DuplicateGroupsOps::dequeueDuplicateRecheckBatch(db_, mode, batch_size)` (or process all recheck first with a small limit).
- For each `file_id` in `recheck_ids`:
  - Skip if already in a group (`getGroupIdForFile(file_id, mode)`).
  - Load artifact; run same logic as for a new file (best group by rep, duplicate vs similar tier, split, add to group or cross-batch / batch_ungrouped). Increment `files_checked` and related counters.
- Then run the existing “new files” batch (id > last_processed_id) as today. Checkpoint still advances only based on new files, not on recheck files (unless we explicitly also advance when processing recheck — usually we don’t, since recheck file_id is already ≤ last_processed_id).

**Edge cases:**

- Key already in another group (e.g. race): `getGroupIdForFile` will see it; skip recheck or treat as no-op.
- Recheck queue grows unbounded: unlikely if we only enqueue on “empty group after split”; optional cleanup job or limit queue size by mode.

---

## 7. Summary

| Item | Action |
|------|--------|
| **Detect “only key left”** | In `trySplitAndRedistribute`, after redistribution, branch on `orig_count == 1`. |
| **Delete original group** | Call `DuplicateGroupsOps::deleteGroup(db_, best_group_id)`; CASCADE removes the last member. |
| **Cache** | `group_representatives.erase(best_group_id)`. |
| **Re-queue (minimal)** | Option A: do nothing else; key is only used again in cross-batch. |
| **Re-queue (full)** | Option C: add `duplicate_recheck_queue`, enqueue key’s `file_id` when deleting empty group; in `processBatch`, process recheck file_ids with same logic as new files, then remove from queue. |

This gives a clear path to both “delete empty group” and “send key image back to the queue to be checked for duplicates” with full re-evaluation if desired.
