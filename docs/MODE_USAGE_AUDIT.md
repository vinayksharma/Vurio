# Mode Parameter Usage Audit

## Summary

This document provides a comprehensive audit of `mode` parameter usage across the codebase after the single-mode refactoring (EMBEDDING mode only).

## Audit Date
2025-10-27

## Status
✅ **PASSED** - All mode parameters are handled correctly

## Key Findings

### 1. Database Schema
- **`duplicate_groups.mode`**: Still exists and is populated with "EMBEDDING"
- **`duplicate_processing_checkpoint.mode`**: Still exists as PRIMARY KEY
- **`image_artifacts.mode`**: Successfully removed during refactoring

### 2. Mode Parameter Flow

#### Duplicate Finder (Main Consumer)
```
findDuplicates()
  └─> mode_str = "EMBEDDING"  (hardcoded)
      └─> processBatch(mode_str, ...)
          ├─> getCheckpoint(db, mode_str)
          ├─> loadFileArtifacts(file_id, mode, ...)  [UNUSED PARAM]
          ├─> getGroupIdForFile(file_id, mode)
          ├─> getThreshold(mode)
          ├─> computeSimilarity(file1, file2, mode)  [UNUSED PARAM]
          ├─> createGroup(..., mode, ...)
          ├─> addToGroup(..., mode, ...)
          └─> upsertCheckpoint(db, mode, ...)
```

#### Database Layer
```
DuplicateGroupsOps::createGroup(db, mode, ...)
  └─> SQL INSERT with mode parameter

DuplicateGroupsOps::getCheckpoint(db, mode)
  └─> SQL SELECT WHERE mode = ?

DuplicateGroupsOps::upsertCheckpoint(db, mode, ...)
  └─> SQL INSERT/UPDATE with mode parameter

DuplicateGroupsOps::getStats(db, mode)
  └─> SQL queries filtered by mode
```

### 3. Unused Parameters (Correctly Marked)

Two functions receive `mode` but don't use it anymore:

1. **`loadFileArtifacts(int file_id, const std::string & /* mode */, ...)`**
   - Previously used to filter `image_artifacts` by mode
   - Mode column removed from `image_artifacts` table
   - Parameter kept for backward compatibility, marked as unused

2. **`computeSimilarity(..., const std::string & /* mode */)`**
   - Previously used to select similarity calculation method
   - Now always uses embedding similarity (only method)
   - Parameter kept for function signature compatibility

### 4. Mode Value Propagation

All mode values are **"EMBEDDING"** (hardcoded or default):

- `duplicate_finder.cpp:97`: `mode_str = "EMBEDDING"`
- `duplicate_finder.cpp:1012`: `mode = "EMBEDDING"`
- `server.cpp:228`: `server_mode_ = "EMBEDDING"`
- `unified_observable_config.hpp:62`: Default = `ServerMode::EMBEDDING`

### 5. SQL Query Analysis

#### Queries That Use Mode (CORRECT)
```sql
-- Duplicate groups
SELECT * FROM duplicate_groups WHERE mode = ?
INSERT INTO duplicate_groups(mode, ...)

-- Checkpoints
SELECT * FROM duplicate_processing_checkpoint WHERE mode = ?
INSERT INTO duplicate_processing_checkpoint(mode, ...)
```

#### Queries That Previously Used Mode (FIXED)
```sql
-- Previously: image_artifacts.mode was queried
-- Now: mode column removed, query simplified
SELECT * FROM image_artifacts WHERE file_path = ?
```

### 6. Function Signatures

All function signatures maintain the `mode` parameter for:
- **Backward compatibility** (existing callers)
- **Future extensibility** (if modes are re-introduced)
- **Type safety** (consistent parameter lists)

### 7. Test Coverage

Tests have been updated to:
- Use `ServerMode::EMBEDDING` exclusively
- Remove FAST/BALANCED mode test cases
- Remove mode-specific overloads

## Recommendations

### ✅ No Changes Required

The current implementation is **correct** and **consistent**:

1. Mode is properly propagated through the call chain
2. Database operations correctly use mode for filtering
3. Unused parameters are properly marked
4. Single-mode operation is enforced throughout

### Future Considerations

If additional modes are ever re-introduced:
1. Re-enable mode checking in `loadFileArtifacts` (add back `WHERE mode = ?`)
2. Re-enable mode selection in `computeSimilarity` (switch on mode)
3. Update `image_artifacts` schema to include `mode` column
4. Update config to support multiple modes

## Conclusion

✅ **All mode parameters are handled correctly**
✅ **No breaking changes detected**
✅ **Code is ready for production use**
