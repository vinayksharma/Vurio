# Remaining Refactoring Steps - Detailed Implementation Guide

## Current Status

7 out of 15 phases complete (~46% by count, ~30% by code volume).

## Critical Files Needing Immediate Attention

### 1. src/media_processors/media_processor.cpp (HIGHEST PRIORITY)

**Status**: Partially started
**Lines**: 1423
**Complexity**: HIGH - 46 mode-related references

**Required Changes**:

#### A. Remove ServerMode parameter from all ScannedFilesOps calls

Replace pattern:

```cpp
// OLD:
ScannedFilesOps::markProcessed(*db_manager, file_path, server_mode, state);
ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path, server_mode, state);
ProcessingErrorsOps::insertError(*db_manager, file_path, server_mode, code, msg, source);

// NEW:
ScannedFilesOps::markProcessed(*db_manager, file_path, state);
ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path, state);
ProcessingErrorsOps::insertError(*db_manager, file_path, code, msg, source);
```

**Locations** (approximate line numbers):

- Lines 186, 198, 271, 280, 315, 327, 357, 393, 412, 438-476 (multiple)
- Lines 572, 580, 587, 614, 619-665 (multiple)
- Lines 761, 769, 776, 803, 808-854 (multiple)

#### B. Remove mode switch statements for current_status checks

Replace pattern:

```cpp
// OLD:
int current_status = 0;
switch (server_mode_copy)
{
case ServerMode::FAST:
    current_status = current_file->processed_fast;
    break;
case ServerMode::BALANCED:
    current_status = current_file->processed_balanced;
    break;
case ServerMode::QUALITY:
    current_status = current_file->processed_quality;
    break;
}

// NEW:
int current_status = current_file->processed;
```

**Locations**: Lines 224-235, 255-266, 525-536, 556-567, 714-725, 745-756, 981-990

#### C. Remove mode switch for processor dispatch

Replace pattern:

```cpp
// OLD (Lines 374-386):
switch (server_mode_copy)
{
case ServerMode::FAST:
    processing_success = image_processor.ProcessFast(processing_file_path, file_path_copy, *db_manager, config_manager);
    break;
case ServerMode::BALANCED:
    processing_success = image_processor.ProcessBalanced(processing_file_path, file_path_copy, *db_manager, config_manager);
    break;
case ServerMode::QUALITY:
    processing_success = image_processor.ProcessQuality(processing_file_path, file_path_copy, *db_manager, config_manager);
    break;
default:
    processing_success = image_processor.ProcessFast(processing_file_path, file_path_copy, *db_manager, config_manager);
}

// NEW:
processing_success = image_processor.Process(processing_file_path, file_path_copy, *db_manager, config_manager);
```

**Similar replacements needed**:

- Video processor dispatch (Lines 595-607)
- Audio processor dispatch (Lines 784-796)

#### D. Remove server_mode_copy variable

Remove these lines:

```cpp
// OLD:
ServerMode server_mode_copy = server_mode;

// NEW:
// (delete line entirely)
```

**Locations**: Lines 207, 511, 700

#### E. Remove getCurrentServerMode() method

Delete lines 1217-1220:

```cpp
// DELETE ENTIRELY:
ServerMode MediaProcessor::getCurrentServerMode() const
{
    return config_manager_->getServerMode("server.mode", ServerMode::FAST);
}
```

#### F. Update getCurrentMediaTypeMessage

Replace lines 902-916:

```cpp
// OLD:
ServerMode current_mode = getCurrentServerMode();

if (current_mode == ServerMode::FAST)
{
    logger.debug("Using FAST mode (pHash) for %s", category_str);
}
else if (current_mode == ServerMode::BALANCED)
{
    logger.debug("Using BALANCED mode (ORB features) for %s", category_str);
}
else if (current_mode == ServerMode::QUALITY)
{
    logger.debug("Using QUALITY mode (CLIP embeddings) for %s", category_str);
}

// NEW:
logger.debug("Using embedding-based processing for %s", category_str);
```

**Automated Approach**: Use sed/awk for batch replacements:

```bash
# Example sed commands (test on copy first!):
sed -i 's/ScannedFilesOps::markProcessed(\*[^,]*, [^,]*, server_mode_copy, /ScannedFilesOps::markProcessed(*database_manager_, file_path_copy, /g' src/media_processors/media_processor.cpp
```

---

### 2. src/database/scanned_files/scanned_files_ops.cpp (CRITICAL)

**Status**: NOT STARTED (header updated)
**Lines**: 917
**Complexity**: VERY HIGH - Complete rewrite needed

**Strategy**: Create new implementation from scratch based on updated header.

**Key Functions to Rewrite**:

1. `ensureTable()` - Update index creation (lines 13-46)
2. `bindUpsertParams()` - Use single `processed`, `links` fields (lines 48-78)
3. `getByPath()` - Update RecordSet extraction (lines 113-147)
4. `listAll()` - Update RecordSet extraction
5. `countProcessed()` - Remove ServerMode parameter
6. `countError()` - Remove ServerMode parameter
7. `countQueued()` - Remove ServerMode parameter
8. `markProcessed()` - Remove ServerMode parameter (lines 428-464)
9. `markProcessedWithEscalation()` - Remove ServerMode logic (lines 466-523)
10. `setLinks()` - Remove ServerMode switch (lines 537-564)
11. `getLinks()` - Remove ServerMode switch (lines 620-650)
12. `listUnprocessed()` - Remove ServerMode switch (lines 652-705)
13. `resetAllErrors()` - Remove ServerMode switch (lines 783-827)

**Template for markProcessed**:

```cpp
bool ScannedFilesOps::markProcessed(DatabaseManager &db, const std::string &file_path, int state)
{
    try
    {
        auto lease = db.acquireSessionLease();
        Session &sess = lease.get();
        Statement stmt(sess);
        int flag = state;
        std::string p = file_path;
        stmt << std::string(SQL::kUpdateProcessed), Keywords::use(flag), Keywords::use(p), Keywords::now;
        Poco::Logger::get("ScannedFilesOps").debug("Successfully updated file " + file_path + " to state " + std::to_string(state));
        return true;
    }
    catch (const std::exception &e)
    {
        Poco::Logger::get("ScannedFilesOps").error("Failed to mark file " + file_path + " with state " + std::to_string(state) + ": " + e.what());
        return false;
    }
}
```

---

### 3. src/orchestration/duplicate_finder.cpp (CRITICAL)

**Status**: NOT STARTED
**Lines**: 1144
**Complexity**: HIGH - Core duplicate detection logic

**Required Changes**:

#### A. Update member variable initialization (Lines 52-65)

```cpp
// OLD:
fast_threshold_ = cfg_->getPropertyValue<double>("duplicates.fast.threshold", 0.90);
balanced_threshold_ = cfg_->getPropertyValue<double>("duplicates.balanced.threshold", 0.30);
quality_threshold_min_ = cfg_->getPropertyValue<double>("duplicates.quality.threshold.min", 0.94);
quality_threshold_max_ = cfg_->getPropertyValue<double>("duplicates.quality.threshold.max", 0.98);
fast_min_hash_size_ = cfg_->getPropertyValue<int>("duplicates.fast.minHashSize", 64);
balanced_min_good_matches_ = cfg_->getPropertyValue<int>("duplicates.balanced.minGoodMatches", 15);
balanced_ratio_test_threshold_ = cfg_->getPropertyValue<double>("duplicates.balanced.ratioTestThreshold", 0.75);
quality_min_confidence_ = cfg_->getPropertyValue<double>("duplicates.quality.minConfidence", 0.90);

// NEW:
similarity_threshold_ = cfg_->getPropertyValue<double>("duplicates.threshold", 0.94);
```

#### B. Remove mode retrieval (Lines 105-106)

```cpp
// OLD:
std::string mode_str = cfg_->getPropertyValue<std::string>("server.mode", "FAST");
std::string mode = mode_str; // Convert to std::string for consistency

// NEW:
std::string mode = "EMBEDDING"; // Single mode
```

#### C. Remove mode-specific query conditions (Lines 256-267)

```cpp
// OLD:
if (mode == "FAST")
{
    query += "processed_fast = 2 ";
}
else if (mode == "BALANCED")
{
    query += "processed_balanced = 2 ";
}
else // QUALITY
{
    query += "processed_quality = 2 ";
}

// NEW:
query += "processed = 2 ";
```

**Similar replacements**: Lines 379-384

#### D. Simplify computeSimilarity (Lines 808-820)

```cpp
// OLD:
if (mode == "FAST")
{
    sim = SimilarityCalculator::computePhashSimilarity(ph1, ph2);
}
else if (mode == "BALANCED")
{
    sim = SimilarityCalculator::computeFeatureMatchSimilarity(feat1, feat2,
        balanced_ratio_test_threshold_, balanced_min_good_matches_);
}
else // QUALITY
{
    sim = SimilarityCalculator::computeEmbeddingSimilarity(emb1, emb2, embedding_dim_);
}

// NEW:
sim = SimilarityCalculator::computeEmbeddingSimilarity(emb1, emb2, embedding_dim_);
```

#### E. Remove getThreshold() complexity (Lines 992-1003)

```cpp
// DELETE getThreshold() method entirely, use similarity_threshold_ directly
```

#### F. Simplify config change handlers (Lines 1023-1084)

```cpp
// DELETE all FAST/BALANCED threshold handlers
// KEEP only:
else if (event.key == "duplicates.threshold")
{
    double new_threshold = cfg_->getPropertyValue<double>(event.key, 0.94);
    if (new_threshold < similarity_threshold_)
    {
        // Threshold relaxed, delete groups and reset
        ...
    }
    similarity_threshold_ = new_threshold;
}
```

---

### 4. src/database/processing_errors_ops.cpp

**Status**: NOT STARTED
**Estimated Complexity**: MEDIUM

**Required Changes**:

- Remove `ServerMode` parameter from `insertError()`
- Update SQL queries to remove mode column references
- Update all call sites

---

### 5. src/database/image_artifacts_ops.cpp

**Status**: NOT STARTED
**Estimated Complexity**: MEDIUM

**Required Changes**:

- Remove `mode` parameter from all methods
- Remove phash and features methods (keep only embedding methods)
- Update SQL queries for single-column schema

---

### 6. src/orchestration/files_manager.cpp

**Status**: NOT STARTED
**Estimated Complexity**: LOW

**Required Changes** (Lines 203-213):

```cpp
// OLD:
row.processed_fast = 0;
row.processed_balanced = 0;
row.processed_quality = 0;

// NEW:
row.processed = 0;
```

---

### 7. Web Handlers (Multiple Files)

**Status**: NOT STARTED
**Estimated Complexity**: MEDIUM

**Files**:

- `src/core/webserver/web_handlers_duplicates.cpp`
- `src/core/webserver/web_handlers_reset_errors.cpp`
- `src/core/webserver/web_handlers_server_status.cpp`

**Changes**:

- Remove `mode` query parameter handling
- Update status reporting
- Simplify API responses

---

### 8. Tests (Multiple Files)

**Status**: NOT STARTED
**Estimated Complexity**: HIGH - Many tests will break

**Strategy**:

1. Run build, collect compilation errors
2. Fix test files one by one
3. Remove mode-specific test cases
4. Update assertions for single-column schema

**Files to Update** (minimum):

- `tests/unit/test_media_processor.cpp`
- `tests/unit/test_scanned_files_ops.cpp`
- `tests/unit/test_processing_errors_ops.cpp`
- Any duplicate finder tests

---

## Recommended Implementation Order

1. **Complete media_processor.cpp** (blocks everything)
2. **Complete scanned_files_ops.cpp** (blocks compilation)
3. **Complete duplicate_finder.cpp** (core functionality)
4. **Update files_manager.cpp** (quick win)
5. **Update processing_errors_ops.cpp**
6. **Update image_artifacts_ops.cpp**
7. **Attempt build**, fix compilation errors
8. **Update web handlers**
9. **Update tests** (after successful build)
10. **Update OpenAPI spec**

## Build and Test Cycle

```bash
# 1. Attempt build
./build.sh 2>&1 | tee build_errors.log

# 2. Fix top 10 errors, repeat

# 3. Once building:
./rebuild

# 4. Fix test failures one by one
```

## Risk Mitigation

- **Backup database before testing**
- **Keep detailed notes of changes**
- **Test incrementally with small media sets**
- **Have rollback plan ready**

## Estimated Time Remaining

- Media processor completion: 2-3 hours
- ScannedFilesOps rewrite: 2-3 hours
- DuplicateFinder update: 1-2 hours
- Other files: 1-2 hours
- Build/test cycle: 2-3 hours
- **Total**: 8-13 hours

## Success Criteria

- [ ] Project builds without errors
- [ ] All unit tests pass
- [ ] Can scan and index media files
- [ ] Can process images with CLIP embeddings
- [ ] Can detect duplicates
- [ ] API endpoints work correctly
- [ ] No mode references in codebase
- [ ] Documentation updated
