# Single-Mode Refactoring Progress

## Objective

Simplify the media deduplication server to use only CLIP embedding-based processing (formerly "QUALITY" mode), removing FAST (pHash) and BALANCED (ORB) modes entirely.

## Completed Phases

### ✅ Phase 1: Configuration Files

**Status**: COMPLETE

**Files Modified**:

- `config/config.yaml`

  - Removed `server.mode`
  - Removed all `media.image.fast.*` settings
  - Removed all `media.image.balanced.*` settings
  - Renamed `media.image.quality.*` → `media.image.*`
  - Removed `duplicates.fast.*` settings
  - Removed `duplicates.balanced.*` settings
  - Renamed `duplicates.quality.threshold.min` → `duplicates.threshold`
  - Removed `duplicates.quality.threshold.max` and `duplicates.quality.minConfidence`

- `src/config/config_manager_factory.cpp`
  - Removed `server.mode` property
  - Removed FAST/BALANCED property registrations
  - Renamed QUALITY properties to standard names
  - Simplified threshold configuration

### ✅ Phase 2: Enums and Type System

**Status**: COMPLETE

**Files Modified**:

- `include/config/config_enums.hpp`
  - Simplified `ServerMode` enum to single `EMBEDDING` value
  - Updated `parseServerMode()` to always return `EMBEDDING`
  - Updated `toString()` to always return "EMBEDDING"

### ✅ Phase 3: Database Schema

**Status**: COMPLETE

**Files Modified**:

- `include/database/sql_constants.hpp`
  - `scanned_files` table: Renamed `processed_fast/balanced/quality` → `processed`
  - `scanned_files` table: Renamed `links_fast/balanced/quality` → `links`
  - `image_artifacts` table: Removed `mode` column, removed pHash and features columns
  - Removed mode-specific indexes
  - Updated all INSERT/UPDATE/SELECT queries
  - Simplified count queries

**Database Migration Required**:

- Drop old database and recreate tables from scratch
- Or run manual migration scripts (not created yet)

### ✅ Phase 4: ScannedFilesOps

**Status**: COMPLETE (Header only)

**Files Modified**:

- `include/database/scanned_files_ops.hpp`
  - Updated `ScannedFileRow` struct: single `processed` and `links` fields
  - Removed `ServerMode` parameters from all methods
  - Simplified API surface

**Files Still Need Work**:

- `src/database/scanned_files/scanned_files_ops.cpp` (917 lines) - needs complete rewrite

### ✅ Phase 5: Remove FAST/BALANCED Pipelines

**Status**: COMPLETE

**Files Deleted**:

- `src/media_processors/image/pipelines/fast_pipeline.cpp`
- `src/media_processors/image/pipelines/balanced_pipeline.cpp`
- `include/media_processors/image/pipelines/fast_pipeline.hpp`
- `include/media_processors/image/pipelines/balanced_pipeline.hpp`

### ✅ Phase 6: ImageProcessor

**Status**: COMPLETE

**Files Modified**:

- `src/media_processors/image_processor.cpp`

  - Removed `ProcessFast()` and `ProcessBalanced()`
  - Renamed `ProcessQuality()` → `Process()`
  - Simplified to single processing method

- `include/media_processors/image_processor.hpp`
  - Updated interface to single `Process()` method
  - Updated documentation

### ✅ Phase 7: VideoProcessor and AudioProcessor

**Status**: COMPLETE

**Files Modified**:

- `src/media_processors/video_processor.cpp` - Single `Process()` method
- `src/media_processors/audio_processor.cpp` - Single `Process()` method
- `include/media_processors/video_processor.hpp` - Updated interface
- `include/media_processors/audio_processor.hpp` - Updated interface

## Remaining Phases

### ❌ Phase 8: MediaProcessor

**Status**: NOT STARTED

**Files to Modify**:

- `src/media_processors/media_processor.cpp` (1423 lines)

  - Remove all `ServerMode` switches
  - Replace `getCurrentServerMode()` calls
  - Call `ImageProcessor::Process()` directly instead of mode-specific methods
  - Remove mode parameter passing
  - Update error handling

- `include/media_processors/media_processor.hpp`
  - Remove ServerMode references

**Key Changes**:

- Lines 168-186: Remove `getCurrentServerMode()` call
- Lines 224-270: Remove mode switch for checking processed status
- Lines 374-386: Replace mode switch with direct `Process()` call
- Similar changes throughout file for video/audio

### ❌ Phase 9: DuplicateFinder

**Status**: NOT STARTED

**Files to Modify**:

- `src/orchestration/duplicate_finder.cpp` (1144 lines)

  - Remove `fast_threshold_`, `balanced_threshold_` member variables
  - Rename `quality_threshold_min_` → `similarity_threshold_`
  - Remove `quality_threshold_max_`, `quality_min_confidence_`
  - Remove `fast_min_hash_size_`, `balanced_*` parameters
  - Remove `getThreshold()` mode logic
  - Update config key subscriptions
  - Remove mode-specific queries (lines 256-267, 379-384, etc.)
  - Update similarity calculation (lines 808-820)

- `include/orchestration/duplicate_finder.hpp`
  - Update member variables
  - Update method signatures

**Key Changes**:

- Lines 52-65: Simplify initialization
- Lines 105-106: Remove mode retrieval
- Lines 256-267: Remove mode conditionals
- Lines 808-820: Use only embedding similarity
- Lines 992-1003: Remove `getThreshold()` complexity
- Lines 1023-1084: Simplify config change handling

### ❌ Phase 10: FilesManager

**Status**: NOT STARTED

**Files to Modify**:

- `src/orchestration/files_manager.cpp`
  - Update `ScannedFileRow` initialization (lines 203-213)
  - Use single `processed` field instead of three

### ❌ Phase 11: Remove Unused Similarity Calculators

**Status**: NOT STARTED

**Files to Review/Modify**:

- `src/media_processors/similarity/similarity_calculator.cpp`

  - Keep `computeEmbeddingSimilarity()`
  - Consider removing `computePhashSimilarity()` and `computeFeatureMatchSimilarity()`
  - Or keep for future use

- `include/media_processors/similarity/similarity_calculator.hpp`
  - Update interface

### ❌ Phase 12: Web Handlers

**Status**: NOT STARTED

**Files to Modify**:

- `src/core/webserver/web_handlers_duplicates.cpp`

  - Remove `mode` query parameter from `/api/v1/duplicates/reset`
  - Simplify handler logic

- `include/core/web/web_handlers_duplicates.hpp`

  - Update documentation

- `src/core/webserver/web_handlers_server_status.cpp`

  - Update status reporting to use single processed column

- `src/core/webserver/web_handlers_reset_errors.cpp`
  - Remove mode parameter

### ❌ Phase 13: Update Tests

**Status**: NOT STARTED

**Test Files to Modify/Remove**:

- `tests/unit/test_media_processor.cpp`
  - Remove mode-specific tests
  - Update to use `Process()` methods
- `tests/unit/test_scanned_files_ops.cpp`

  - Update for single `processed` column
  - Remove mode-specific assertions

- `tests/unit/test_processing_errors_ops.cpp`

  - Update for single mode

- `tests/unit/test_duplicate_finder.cpp` (if exists)

  - Update threshold testing

- `tests/examples/quality_smoke.cpp`
  - Rename or update

**Files to Remove**:

- Any FAST/BALANCED specific test files

### ❌ Phase 14: Build and Test

**Status**: NOT STARTED

**Actions**:

1. Run full build: `./build.sh`
2. Fix compilation errors
3. Run unit tests: `./rebuild`
4. Fix test failures
5. Manual testing of core functionality

### ❌ Phase 15: Update OpenAPI Specification

**Status**: NOT STARTED

**Files to Modify**:

- `src/core/webserver/static/api/openapi.json`
  - Remove `mode` parameters from duplicate endpoints
  - Update response schemas
  - Update descriptions

## Critical Implementation Files Still Needing Work

### High Priority (Blocks Compilation)

1. **src/database/scanned_files/scanned_files_ops.cpp** (917 lines)

   - Complete rewrite needed
   - All functions reference old column names
   - All functions use ServerMode parameter

2. **src/media_processors/media_processor.cpp** (1423 lines)

   - Remove ~50+ mode switches
   - Update processor calls
   - Critical for runtime

3. **src/orchestration/duplicate_finder.cpp** (1144 lines)
   - Remove mode logic
   - Update threshold handling
   - Update queries

### Medium Priority (Needed for Functionality)

4. **src/database/processing_errors_ops.cpp**

   - Update to remove ServerMode parameter

5. **src/database/image_artifacts_ops.cpp**

   - Update for single-mode schema

6. **src/orchestration/files_manager.cpp**

   - Update row initialization

7. **Web handlers** (multiple files)
   - Remove mode parameters

### Low Priority (Nice to Have)

8. **Similarity calculator** - Consider cleanup
9. **Tests** - Will need extensive updates
10. **Documentation** - Update all references

## Estimated Remaining Work

- **Files to modify**: ~30-40 files
- **Lines of code to change**: ~3000-4000 lines
- **Estimated time**: 6-8 hours
- **Risk level**: HIGH (breaking changes, database recreation required)

## Next Steps

1. **Complete ScannedFilesOps implementation** (highest priority)
2. **Update MediaProcessor** (blocks everything)
3. **Update DuplicateFinder** (core functionality)
4. **Attempt compilation** and fix errors
5. **Update remaining files** based on compilation errors
6. **Fix tests** after successful compilation
7. **Manual testing** and validation

## Notes

- Database MUST be deleted and recreated (schema changes)
- All existing processed files will need reprocessing
- All existing duplicate groups will be lost
- This is a BREAKING change with no backward compatibility
- Consider creating a database migration script if preservation of existing data is needed

## Rollback Plan

If issues arise:

1. Git revert to commit before refactoring started
2. Restore database backup
3. Resume normal operations

## Testing Strategy

After implementation:

1. Fresh database creation
2. Register test media location
3. Verify file scanning works
4. Verify image processing works
5. Verify duplicate detection works
6. Verify API endpoints work
7. Performance testing with various image sets
