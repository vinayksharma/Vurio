# Representative File Management Analysis

## Overview

This document explains how the representative file is managed in duplicate groups, including selection criteria, when swapping occurs, and the conditions that trigger updates.

## Initial Representative Selection

### When Creating a New Group

When a duplicate group is first created, the representative is selected using the `selectRepresentative()` function:

**Location:** `src/orchestration/duplicate_finder.cpp:766-795`

```cpp
RepresentativeInfo selectRepresentative(const std::vector<FileArtifact> &members)
{
    RepresentativeInfo best = members[0];

    for (size_t i = 1; i < members.size(); ++i) {
        RepresentativeInfo candidate = members[i];

        if (isBetterRepresentative(candidate, best)) {
            best = candidate;
        }
    }

    return best;
}
```

### Selection Criteria

**Location:** `src/orchestration/duplicate_finder.cpp:797-807`

```cpp
bool isBetterRepresentative(const RepresentativeInfo &a, const RepresentativeInfo &b)
{
    // Priority 1: Larger file size
    if (a.file_size != b.file_size) {
        return a.file_size > b.file_size;  // BIGGER IS BETTER
    }

    // Priority 2: Older date (lexicographic comparison, assumes ISO format)
    return a.created_date < b.created_date;  // OLDER IS BETTER
}
```

**Selection Strategy:**

1. **Primary Factor**: `file_size` - Larger file wins
2. **Tie-Breaker**: `created_date` - Older file wins (lexicographic comparison)

### Configuration

The strategy is configurable (though currently only one implementation exists):

**Config Property:** `duplicates.representative.strategy`
**Default Value:** `"size_then_age"`
**Options:** `"size_then_age"` | `"age_then_size"` (future)

**Location:** `config/config.yaml:185`

```yaml
duplicates.representative.strategy: size_then_age
```

## Dynamic Representative Swapping

### YES - Representatives ARE Swapped!

The system **actively swaps** the representative file when a better candidate is added to an existing group.

### When Swapping Occurs

**Location:** `src/orchestration/duplicate_finder.cpp:869-928`

Every time a new file is added to an existing group, the system:

1. **Loads current representative** from the group
2. **Compares new file** against current representative using `isBetterRepresentative()`
3. **Swaps if better** - new file becomes representative if it's larger or older

```cpp
bool DuplicateFinder::addToGroup(int group_id, const FileArtifact &file, ...)
{
    // Get current group info
    auto group = DuplicateGroupsOps::getGroupById(db_, group_id);

    // Check if new file should be representative
    RepresentativeInfo current_rep;
    current_rep.file_id = group.representative_file_id;
    current_rep.file_path = group.representative_file_path;
    current_rep.file_size = group.representative_file_size;
    current_rep.created_date = group.representative_created_date;

    RepresentativeInfo new_candidate;
    new_candidate.file_id = file.file_id;
    new_candidate.file_path = file.file_path;
    new_candidate.file_size = file.file_size;
    new_candidate.created_date = file.created_date;

    bool should_update_rep = isBetterRepresentative(new_candidate, current_rep);

    // Add member with rep flag
    DuplicateGroupsOps::addMember(db_, group_id, file.file_id, file.file_path,
                                  similarity_score, file.file_size,
                                  file.created_date, should_update_rep);

    // Update representative if needed
    if (should_update_rep) {
        // Clear old representative flag
        DuplicateGroupsOps::updateMemberRepresentativeFlag(db_, group_id,
                                                           current_rep.file_id, false);

        // Update group representative
        DuplicateGroupsOps::updateGroupRepresentative(db_, group_id,
                                                      new_candidate.file_id,
                                                      new_candidate.file_path,
                                                      new_candidate.file_size,
                                                      new_candidate.created_date,
                                                      new_member_count);

        logger.information("Updated group %d representative from file_id %d to %d (bigger/older)",
                          group_id, current_rep.file_id, new_candidate.file_id);
    }
}
```

### Swap Process Steps

When a swap occurs:

1. **Add new member** to `duplicate_members` table with `is_representative = 1`
2. **Clear old flag** in `duplicate_members` for previous representative (`is_representative = 0`)
3. **Update group record** in `duplicate_groups` table with new representative info
4. **Update in-memory cache** so subsequent comparisons use new representative
5. **Log the swap** for debugging/auditing

**Location of flag update:** `src/database/duplicate_groups_ops.cpp:441-474`

```cpp
bool updateMemberRepresentativeFlag(DatabaseManager &db,
                                   int group_id,
                                   int file_id,
                                   bool is_representative)
{
    // SQL: UPDATE duplicate_members
    //      SET is_representative=?
    //      WHERE group_id=? AND file_id=?
}
```

### Cache Update

The in-memory representative cache is also updated to reflect the swap:

**Location:** `src/orchestration/duplicate_finder.cpp:501-508`

```cpp
if (addToGroup(best_group_id, new_file, mode, best_similarity)) {
    // Update representative in our cache if this file is now the rep
    auto group_opt = DuplicateGroupsOps::getGroupById(db_, best_group_id);
    if (group_opt.has_value() && group_opt->representative_file_id == new_file.file_id) {
        group_representatives[best_group_id] = new_file;
        logger.information("Group %d representative swapped to file_id %d (larger/older)",
                          best_group_id, new_file.file_id);
    }
}
```

## Conditions for Swapping

A representative swap occurs when **ALL** of the following conditions are met:

### 1. File is Added to Existing Group

- New file must match the current representative (similarity >= threshold)
- File passes the "all-members check" (similar to ALL existing group members)

### 2. New File Has Larger Size OR Older Date

**Comparison Logic:**

```
IF new_file.size > current_rep.size:
    SWAP → New file becomes representative
ELSE IF new_file.size == current_rep.size:
    IF new_file.created_date < current_rep.created_date:
        SWAP → New file becomes representative
    ELSE:
        NO SWAP → Keep current representative
ELSE:
    NO SWAP → Keep current representative (current is larger)
```

### 3. Database Operations Succeed

All database operations must succeed:

- Adding new member to `duplicate_members`
- Updating old representative flag
- Updating group record in `duplicate_groups`

## Examples

### Example 1: Larger File Triggers Swap

**Initial Group:**

- Representative: `file_100.jpg` (Size: 2.5 MB, Date: 2023-01-15)
- Member: `file_101.jpg` (Size: 2.0 MB, Date: 2023-01-20)

**New File Added:**

- `file_102.jpg` (Size: 3.0 MB, Date: 2023-01-10)

**Result:**

- ✅ **SWAP OCCURS**
- New representative: `file_102.jpg` (3.0 MB > 2.5 MB)
- Old representative becomes regular member

### Example 2: Same Size, Older Date Triggers Swap

**Initial Group:**

- Representative: `file_200.jpg` (Size: 2.0 MB, Date: 2023-06-01)
- Member: `file_201.jpg` (Size: 1.8 MB, Date: 2023-06-05)

**New File Added:**

- `file_202.jpg` (Size: 2.0 MB, Date: 2023-05-15)

**Result:**

- ✅ **SWAP OCCURS**
- New representative: `file_202.jpg` (same size, but older date)
- 2023-05-15 < 2023-06-01 (lexicographic comparison)

### Example 3: Smaller File, No Swap

**Initial Group:**

- Representative: `file_300.jpg` (Size: 5.0 MB, Date: 2023-01-01)
- Member: `file_301.jpg` (Size: 4.5 MB, Date: 2023-01-15)

**New File Added:**

- `file_302.jpg` (Size: 4.0 MB, Date: 2022-12-01)

**Result:**

- ❌ **NO SWAP**
- Keep current representative: `file_300.jpg` (larger size wins)
- New file added as regular member

### Example 4: Same Size, Newer Date, No Swap

**Initial Group:**

- Representative: `file_400.jpg` (Size: 3.0 MB, Date: 2022-01-01)
- Member: `file_401.jpg` (Size: 2.5 MB, Date: 2022-06-01)

**New File Added:**

- `file_402.jpg` (Size: 3.0 MB, Date: 2023-01-01)

**Result:**

- ❌ **NO SWAP**
- Keep current representative: `file_400.jpg` (older date wins)
- 2022-01-01 < 2023-01-01

## Database Schema

### duplicate_groups Table

Stores current representative info:

```sql
CREATE TABLE duplicate_groups (
    id INTEGER PRIMARY KEY,
    mode TEXT NOT NULL,
    representative_file_id INTEGER NOT NULL,      -- Current rep
    representative_file_path TEXT NOT NULL,       -- Current rep path
    representative_file_size INTEGER NOT NULL,    -- Current rep size
    representative_created_date TEXT NOT NULL,    -- Current rep date
    similarity_threshold REAL NOT NULL,
    member_count INTEGER NOT NULL,
    created_at TIMESTAMP,
    updated_at TIMESTAMP  -- Updated when rep swaps
);
```

### duplicate_members Table

Tracks which member is representative:

```sql
CREATE TABLE duplicate_members (
    group_id INTEGER NOT NULL,
    file_id INTEGER NOT NULL,
    file_path TEXT NOT NULL,
    similarity_score REAL NOT NULL,
    file_size INTEGER NOT NULL,
    created_date TEXT NOT NULL,
    is_representative BOOLEAN DEFAULT 0,  -- Only ONE member has is_representative=1
    added_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (group_id, file_id)
);
```

## Performance Impact

### Swap Frequency

Representative swaps are **relatively rare** because:

1. Files are processed in order (smaller IDs first)
2. Larger/higher quality images often have smaller IDs (original imports)
3. Most new files are compressed versions or lower quality

### Database Operations per Swap

When a swap occurs:

1. 1× INSERT into `duplicate_members` (new member)
2. 1× UPDATE on `duplicate_members` (clear old flag)
3. 1× UPDATE on `duplicate_groups` (new representative info)

**Total: 3 database operations** per swap

## Rationale: Why Swap Representatives?

### 1. **Quality Preservation**

The representative should be the **highest quality** version:

- Larger files typically have higher resolution
- Original files are usually larger than edited versions

### 2. **User Experience**

When browsing duplicates, users see the representative:

- Show the best quality version as the "keeper"
- Lower quality versions shown as "delete candidates"

### 3. **Storage Optimization**

When auto-deleting duplicates (future feature):

- Keep the largest/best file
- Delete smaller compressed versions

### 4. **Metadata Accuracy**

Older files are often the originals:

- Original has correct EXIF data
- Edited versions may have stripped metadata

## Logging and Debugging

### Log Messages

Representative swaps are logged at **INFO** level:

```
[INFO] Updated group 123 representative from file_id 456 to 789 (bigger/older)
[INFO] Group 123 representative swapped to file_id 789 (larger/older)
```

### Query Current Representative

```sql
-- Get representative for a group
SELECT representative_file_id, representative_file_path,
       representative_file_size, representative_created_date
FROM duplicate_groups
WHERE id = ?;

-- Find which member is marked as representative
SELECT file_id, file_path, file_size, created_date
FROM duplicate_members
WHERE group_id = ? AND is_representative = 1;
```

### Verify Consistency

Both should match:

```sql
-- Check for mismatches
SELECT dg.id, dg.representative_file_id, dm.file_id
FROM duplicate_groups dg
LEFT JOIN duplicate_members dm
    ON dg.id = dm.group_id AND dm.is_representative = 1
WHERE dg.representative_file_id != dm.file_id;
```

## Future Enhancements

### Potential Improvements

1. **Alternative Strategies**:

   - `age_then_size`: Prioritize oldest file over largest
   - `resolution_then_size`: Use image dimensions instead of file size
   - `quality_score`: Compute quality metric (sharpness, compression)

2. **Manual Override**:

   - API endpoint to manually set representative
   - UI button: "Make this the representative"

3. **Batch Re-evaluation**:

   - Periodically re-evaluate all groups
   - Fix representatives that were added in wrong order

4. **Smart Selection**:
   - Prefer files with complete metadata
   - Avoid files with "edited" in filename
   - Check image dimensions vs file size (avoid bloated files)

## Related Files

### Core Implementation

- `src/orchestration/duplicate_finder.cpp:766-928` - Selection and swap logic
- `include/orchestration/duplicate_finder.hpp:161-218` - Function declarations
- `src/database/duplicate_groups_ops.cpp:72-163` - Database operations

### Configuration

- `config/config.yaml:185` - Representative strategy setting
- `src/config/config_manager_factory.cpp:386-388` - Property definition

### Documentation

- `docs/duplicate_detection_architecture.md:114-128` - Algorithm description
- `docs/DUPLICATE_GROUPING_FIX.md` - Historical context

## Summary

**Key Takeaways:**

✅ Representatives **ARE** swapped dynamically  
✅ Swap occurs when larger or older file is added  
✅ Priority: **Size > Date** (bigger first, then older)  
✅ Database and cache both updated on swap  
✅ Old representative becomes regular member  
✅ Logged for debugging and auditing

**Swap Conditions:**

1. New file added to existing group
2. New file has larger size **OR** (same size + older date)
3. Database operations succeed

**Purpose:**

- Keep highest quality version as representative
- Better user experience when browsing duplicates
- Optimal for future auto-deletion features
