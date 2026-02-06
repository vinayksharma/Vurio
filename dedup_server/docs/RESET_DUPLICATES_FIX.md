# Reset Duplicates Bug Fix

## Issue

The reset duplicates functionality was not actually resetting duplicate groups when called via the API endpoint `DELETE /api/v1/duplicates/reset`.

## Root Cause

The system was refactored to use only **"EMBEDDING"** mode for duplicate detection (previously there were three modes: FAST, BALANCED, and QUALITY). However, the `ResetDuplicatesHandler` was not updated during this refactoring and was still attempting to reset the old modes.

### Before Fix

When the reset duplicates API was called:

1. If no mode parameter was provided, it would default to resetting: `["FAST", "BALANCED", "QUALITY"]`
2. The handler would execute `deleteGroupsByMode` for each of these old modes
3. Since all actual duplicate groups were stored under the "EMBEDDING" mode, **no groups were deleted**
4. The API would return success, but no groups were actually reset

### Code Location

- **File**: `src/core/webserver/web_handlers_duplicates.cpp`
- **Function**: `ResetDuplicatesHandler::handleRequest()`
- **Lines**: 188, 216

## Fix Applied

### Changes Made

1. **Updated mode validation** (line 188):

   - Changed from: `if (mode_upper == "FAST" || mode_upper == "BALANCED" || mode_upper == "QUALITY")`
   - Changed to: `if (mode_upper == "EMBEDDING")`

2. **Updated default modes** (line 216):

   - Changed from: `modes_to_reset = {"FAST", "BALANCED", "QUALITY"};`
   - Changed to: `modes_to_reset = {"EMBEDDING"};`

3. **Updated error messages**:

   - Changed from: "Invalid mode parameter. Must be FAST, BALANCED, or QUALITY"
   - Changed to: "Invalid mode parameter. Only EMBEDDING mode is supported"

4. **Updated documentation**:
   - Updated header file comments in `include/core/web/web_handlers_duplicates.hpp`
   - Updated OpenAPI specification in `src/core/webserver/static/api/openapi.json`
   - Updated mode enum from `["FAST", "BALANCED", "QUALITY"]` to `["EMBEDDING"]`

## Testing

All existing unit tests pass successfully:

```bash
$ ./build/bin/all_unit_tests --gtest_filter="DuplicatesResetAPITest.*"
[==========] Running 4 tests from 1 test suite.
[----------] 4 tests from DuplicatesResetAPITest
[ RUN      ] DuplicatesResetAPITest.DeleteGroupsByMode_RemovesOnlySpecifiedMode
[       OK ] DuplicatesResetAPITest.DeleteGroupsByMode_RemovesOnlySpecifiedMode (19 ms)
[ RUN      ] DuplicatesResetAPITest.DeleteGroupsByMode_ClearsGroupsCompletely
[       OK ] DuplicatesResetAPITest.DeleteGroupsByMode_ClearsGroupsCompletely (5 ms)
[ RUN      ] DuplicatesResetAPITest.ResetCheckpoint_ResetsToZero
[       OK ] DuplicatesResetAPITest.ResetCheckpoint_ResetsToZero (3 ms)
[ RUN      ] DuplicatesResetAPITest.ResetAll_ClearsAllModes
[       OK ] DuplicatesResetAPITest.ResetAll_ClearsAllModes (4 ms)
[----------] 4 tests from DuplicatesResetAPITest (31 ms total)
[  PASSED  ] 4 tests.
```

## API Usage

### Reset All Duplicates (Default)

```bash
curl -X DELETE "http://localhost:8080/api/v1/duplicates/reset"
```

### Reset Specific Mode (EMBEDDING)

```bash
curl -X DELETE "http://localhost:8080/api/v1/duplicates/reset?mode=EMBEDDING"
```

### Response

```json
{
  "success": true
}
```

## Shell Script

The shell script `scripts/reset_duplicates.sh` was **NOT affected** by this bug, as it directly executes SQL commands:

```bash
DELETE FROM duplicate_members;
DELETE FROM duplicate_groups;
DELETE FROM duplicate_processing_checkpoint;
```

The shell script deletes all rows regardless of mode, so it would have worked correctly even with the bug present.

## Related Files Modified

1. `src/core/webserver/web_handlers_duplicates.cpp` - Handler implementation
2. `include/core/web/web_handlers_duplicates.hpp` - Handler documentation
3. `src/core/webserver/static/api/openapi.json` - API documentation

## Related Documentation

- `docs/MODE_USAGE_AUDIT.md` - Documents the single-mode refactoring
- `docs/DUPLICATES_API.md` - API documentation for duplicate endpoints
- `docs/duplicate_detection_architecture.md` - System architecture

## Date

2025-10-27
