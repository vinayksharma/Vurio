# Duplicate Grouping Bug Fix - October 2025

## Executive Summary

Fixed critical bug in duplicate detection where **all duplicate groups contained exactly 2 members** instead of grouping all similar files together. The issue was caused by incomplete implementation that only compared files within the same batch and created pairwise groups.

## Problem Statement

### Symptoms

- All 126,920 duplicate groups had exactly 2 members (no groups with 3+)
- Files that should be grouped together were split into multiple pairwise groups
- Example: Files A, B, C, D (all duplicates) created 2 groups of 2 instead of 1 group of 4

### Impact

- Inefficient duplicate management (thousands of small groups instead of consolidated groups)
- Incorrect duplicate statistics
- User confusion when reviewing duplicates
- Wasted storage reporting (counted pairs separately)

## Root Cause Analysis

### Primary Bug: No Cross-Batch Comparison

**Location:** `src/orchestration/duplicate_finder.cpp` lines 241-244

The code **intentionally skipped** loading existing files from previous batches:

```cpp
// For initial implementation, skip loading existing files
// to avoid complex RecordSet issues. We'll compare new files against each other.
// TODO: Optimize by loading existing files for incremental comparison
logger.information("Skipping existing files comparison (comparing new files against each other)");
```

**Result:** `existing_files` vector was empty every batch, so files from previous batches were never compared against new files.

### Secondary Bug: Pairwise Group Creation

**Location:** `src/orchestration/duplicate_finder.cpp` lines 302-324

When a match was found, the code created a group with exactly 2 files and immediately broke:

```cpp
for (size_t j = 0; j < i; ++j)
{
    double sim = computeSimilarity(new_file, other_new, mode);
    if (sim >= threshold)
    {
        // Create new group with both files
        std::vector<FileArtifact> group_members = {new_file, other_new};
        int new_group_id = createDuplicateGroup(group_members, mode, threshold);
        duplicates_found += 2;
        groups_created++;
        break;  // ← Stops after first match!
    }
}
```

### Contributing Factor: High Frequency Execution

- `duplicates.finder.intervalMs: 5000` (runs every 5 seconds)
- Image processing is slower, so typically only 1-2 files processed between runs
- With empty `existing_files`, each run could only create pairwise groups

### Configuration Issue: Zero Thresholds

All similarity thresholds were set to 0, making every file match every other file:

```yaml
duplicates.fast.threshold: 0 # Should be 0.90
duplicates.balanced.threshold: 0 # Should be 0.30
duplicates.quality.threshold: 0 # Should be 0.95
```

This made the pairwise bug extremely visible - should have had groups of thousands, got thousands of pairs instead.

## Solution Implemented

### 1. Fixed Existing File Loading

**Change:** Properly load ALL existing processed files with artifacts for cross-batch comparison.

```cpp
// Load existing files for proper cross-batch duplicate detection
try
{
    Statement existing_stmt(sess);
    existing_stmt << existing_query, use(last_processed_id), use(mode_copy);
    existing_stmt.execute();

    Poco::Data::RecordSet existing_rs(existing_stmt);

    for (auto &existing_row : existing_rs)
    {
        FileArtifact artifact;
        // Parse all artifact data from row
        existing_files.push_back(artifact);
    }

    logger.information("Loaded %zu existing files for comparison", existing_files.size());
}
```

### 2. Rewrote Grouping Logic

**New Algorithm:**

For each new file:

1. Check if already in a group (could have been added earlier in batch)
2. Find **ALL** similar files (not just first match)
3. Group similar files by their current group membership
4. Apply logic based on grouping scenario:
   - **No existing groups:** Create new group with ALL similar files
   - **One existing group:** Add new file + any ungrouped similar files to it
   - **Multiple existing groups:** Add to largest group (bridging scenario)

**Key Improvements:**

- Groups ALL similar files together in one pass
- Checks existing group membership before creating new groups
- Handles bridging files that connect multiple groups
- No artificial limitation to pairs

### 3. Added Helper Function

```cpp
std::optional<int> getGroupIdForFile(int file_id, const std::string &mode)
```

Efficiently checks if a file is already in a group to prevent:

- Duplicate group membership
- Unnecessary group creation
- Processing already-grouped files

### 4. Restored Configuration Defaults

```yaml
duplicates.fast.threshold: 0.90 # was 0
duplicates.balanced.threshold: 0.30 # was 0
duplicates.quality.threshold: 0.95 # was 0
```

## Verification Steps

### 1. Before Fix - Database Evidence

```sql
SELECT COUNT(*) as total_groups,
       AVG(member_count) as avg_members,
       MIN(member_count) as min_members,
       MAX(member_count) as max_members
FROM duplicate_groups;

Result: 126920 groups | avg: 2.0 | min: 2 | max: 2
```

**All groups had exactly 2 members!**

### 2. After Fix - Expected Behavior

With proper thresholds and fixed algorithm:

- Groups should have 3+ members when multiple files are similar
- Cross-batch duplicate detection should work
- Bridging files should connect existing groups

### 3. Test Scenarios

**Scenario A: Multiple Similar Files**

- Process files: A, B, C (all similar, threshold >= 0.90)
- Expected: 1 group with 3 members
- Before fix: 2 groups with 2 members each

**Scenario B: Cross-Batch Addition**

- Batch 1: Process A, B → Group 1 {A, B}
- Batch 2: Process C (similar to A and B)
- Expected: Group 1 {A, B, C}
- Before fix: Group 2 {C, ?} or C ungrouped

**Scenario C: Bridging Files**

- Batch 1: A, B → Group 1
- Batch 2: C, D → Group 2
- Batch 3: E (similar to both A and C)
- Expected: E added to larger group
- Before fix: E would create Group 3 with first match only

## Database Migration

Since the bug resulted in 126,920 incorrect pairwise groups, **database reset required**:

```bash
./scripts/reset_duplicates.sh
```

This script:

1. Deletes all duplicate_members
2. Deletes all duplicate_groups
3. Deletes all duplicate_processing_checkpoint entries
4. Vacuums database to reclaim space

After reset, restart server and duplicate finder will rebuild groups correctly.

## Performance Implications

### Complexity Change

**Before:**

- O(B) per batch (only compare within batch)
- Very fast but incorrect

**After:**

- O(B × N) per batch where:
  - B = batch size (default 1000)
  - N = existing processed files
- Slower but correct

### Optimization Strategies

For large datasets (>100K files):

1. **Reduce batch size:** `duplicates.finder.batchSize: 500`
2. **Increase interval:** `duplicates.finder.intervalMs: 3600000` (1 hour)
3. **Monitor memory:** Each batch loads all existing artifacts into memory
4. **Future:** Implement ANN indexing (FAISS/Annoy) for O(log N) lookup

## Files Modified

### Core Implementation

- `src/orchestration/duplicate_finder.cpp` (~150 lines modified)

  - Added `getGroupIdForFile()` helper method
  - Fixed existing file loading logic
  - Completely rewrote grouping algorithm
  - Added `#include <map>` for group tracking

- `include/orchestration/duplicate_finder.hpp` (~10 lines added)
  - Added declaration for `getGroupIdForFile()`

### Configuration

- `config/config.yaml` (3 lines)
  - Restored proper similarity thresholds

### Scripts

- `scripts/reset_duplicates.sh` (new file, ~60 lines)
  - Database reset script with confirmation prompts

### Documentation

- `docs/duplicate_detection_architecture.md` (updated)

  - Fixed algorithm description
  - Added v2 improvements section
  - Updated complexity analysis

- `docs/DUPLICATE_GROUPING_FIX.md` (this file)
  - Comprehensive bug documentation

## Testing Strategy

### Unit Tests Required

1. **Multi-file grouping:** 5 similar files → 1 group with 5 members
2. **Cross-batch addition:** Files added across multiple batches end up in same group
3. **Bridging detection:** File similar to multiple groups handled correctly
4. **Threshold filtering:** Only files >= threshold are grouped
5. **Representative selection:** Correct representative chosen with 3+ members
6. **No false grouping:** Dissimilar files don't get grouped

### Integration Tests

1. **Full workflow:** Process directory with known duplicates
2. **API validation:** Query `/api/v1/duplicates/groups` returns correct groups
3. **Database consistency:** member_count matches actual member count
4. **Checkpoint recovery:** Restart mid-processing resumes correctly

## Lessons Learned

1. **Never defer critical logic:** "TODO for later" comments often never get fixed
2. **Test with realistic data:** 0 thresholds caught the bug but also hid proper behavior
3. **Integration tests matter:** Unit tests alone wouldn't have caught the cross-batch bug
4. **Document assumptions:** The "batch-only" comparison was never documented as a limitation
5. **Performance vs Correctness:** Initial implementation chose speed over correctness

## Future Enhancements

While the current fix resolves the pairwise bug, future optimizations could include:

1. **Complete group merging:** Automatically merge when bridging file connects multiple groups
2. **ANN indexing:** Use FAISS/Annoy for O(log N) similarity search
3. **Spatial hashing:** Bucket files by pHash prefix for faster lookups
4. **Incremental loading:** Only load files from existing groups, not all files
5. **Parallel processing:** Multi-threaded similarity computation

## References

- Original bug audit: See conversation history from October 20, 2025
- Architecture docs: `docs/duplicate_detection_architecture.md`
- API documentation: `docs/DUPLICATES_API.md`
- Configuration reference: `docs/CONFIGURATION_REFERENCE.md`

---

**Fixed by:** AI Assistant (Claude)  
**Date:** October 20, 2025  
**Severity:** Critical - all duplicate groups incorrect  
**Database reset:** Required
