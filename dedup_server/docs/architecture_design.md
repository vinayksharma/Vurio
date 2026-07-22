# Duplicate Detection Architecture

This document describes the duplicate detection system in the Media Dedup Server, covering the incremental processing design, similarity algorithms, representative selection, and database schema.

## Overview

The duplicate detection system continuously monitors processed media files and identifies duplicates based on similarity of their artifacts (perceptual hashes, features, or embeddings depending on the server mode).

### Key Features

- **Incremental Processing**: Only processes newly completed files, not entire dataset
- **Mode-Specific**: Different similarity algorithms for FAST, BALANCED, and QUALITY modes
- **Smart Representative Selection**: Automatically selects best file (biggest size, oldest date)
- **TPM Integration**: Registered as scheduled job with configurable thread share
- **Checkpoint-Based**: Tracks progress to resume after restarts
- **Live Configuration**: All thresholds and settings update without restart

## Architecture Components

### 1. DuplicateFinder Service

**Location**: `src/orchestration/duplicate_finder.cpp`

Main service that orchestrates duplicate detection:

- Runs periodically as a scheduled job
- Loads checkpoint to track last processed file ID
- Queries for newly processed files since checkpoint
- Computes similarity against existing processed files
- Creates/updates duplicate groups
- Selects and updates group representatives
- Updates checkpoint after each batch

**Initialization**: Called from `ServerInitializer::initializeSchedulerAndFiles()`

**Registered Job**: `"duplicateFinder"` with default 1-hour interval

### 2. Database Schema

#### duplicate_groups Table

```sql
CREATE TABLE duplicate_groups (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    mode TEXT NOT NULL,                        -- FAST/BALANCED/QUALITY
    representative_file_id INTEGER NOT NULL,    -- FK to scanned_files.id
    representative_file_path TEXT NOT NULL,
    representative_file_size INTEGER NOT NULL,
    representative_created_date TEXT NOT NULL,
    similarity_threshold REAL NOT NULL,         -- Threshold used to form group
    member_count INTEGER NOT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);
```

#### duplicate_members Table

```sql
CREATE TABLE duplicate_members (
    group_id INTEGER NOT NULL,                 -- FK to duplicate_groups.id
    file_id INTEGER NOT NULL,                  -- FK to scanned_files.id
    file_path TEXT NOT NULL,
    similarity_score REAL NOT NULL,            -- Similarity to representative
    file_size INTEGER NOT NULL,
    created_date TEXT NOT NULL,
    is_representative BOOLEAN DEFAULT 0,
    added_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (group_id, file_id)
);
```

#### duplicate_processing_checkpoint Table

```sql
CREATE TABLE duplicate_processing_checkpoint (
    mode TEXT PRIMARY KEY,                     -- FAST/BALANCED/QUALITY
    last_processed_id INTEGER NOT NULL,        -- Last scanned_files.id processed
    last_run_timestamp TIMESTAMP,
    files_checked INTEGER NOT NULL,
    duplicates_found INTEGER NOT NULL,
    groups_created INTEGER NOT NULL,
    groups_updated INTEGER NOT NULL
);
```

### 3. Similarity Calculator

**Location**: `src/media_processors/similarity/similarity_calculator.cpp`

Provides three similarity computation methods:

#### FAST Mode: Perceptual Hash (pHash)

- **Algorithm**: Hamming distance on 64-bit hashes
- **Similarity**: `1.0 - (hamming_distance / 64)`
- **Threshold**: 0.90 (default) - ~6 bits difference allowed
- **Use Case**: Identical or near-identical images

#### BALANCED Mode: Feature Matching

- **Algorithm**: ORB/SIFT feature matching with Lowe's ratio test
- **Similarity**: `good_matches / max(features1, features2)`
- **Threshold**: 0.30 (default) - 30% features must match
- **Use Case**: Similar images with different crops/angles

#### QUALITY Mode: Deep Embeddings

- **Algorithm**: Cosine similarity on CLIP embeddings
- **Similarity**: `dot(emb1, emb2) / (||emb1|| * ||emb2||)`
- **Threshold**: 0.95 (default) - very similar content
- **Use Case**: Perceptually similar images

### 4. Representative Selection Strategy

**Priority**: `file_size` (bigger) → `created_date` (older)

When a file is added to a group, the system checks if it should become the new representative:

```cpp
bool isBetterRepresentative(File A, File B) {
    if (A.file_size != B.file_size)
        return A.file_size > B.file_size;  // Bigger is better
    return A.created_date < B.created_date;  // Older is better
}
```

**Dynamic Updates**: Representative can change as new files are added to the group.

**Representative Swapping:**

- Representatives **ARE** swapped dynamically when a better candidate is added
- Swap occurs when new file has larger size OR (same size + older date)
- Only files with similarity >= max threshold can become representatives
- Database and in-memory cache both updated on swap
- Old representative becomes regular member (`is_representative = 0`)

**Swap Process:**

1. Add new member to `duplicate_members` with `is_representative = 1`
2. Clear old representative flag (`is_representative = 0`)
3. Update group record in `duplicate_groups` with new representative info
4. Update in-memory representative cache

**Configuration:**

```yaml
duplicates.representative.strategy: size_then_age # Currently only option
```

## Processing Flow

### 1. Scheduled Execution

```
Every N ms (configurable: duplicates.finder.intervalMs)
  ├─ Check if enabled (duplicates.finder.enabled)
  ├─ Get current server mode (server.mode)
  ├─ Load checkpoint for mode
  └─ Process batches until no more new files
```

### 2. Batch Processing - Representative-Based Algorithm

```
For each batch of N files (duplicates.finder.batchSize):
  ├─ Query files with id > last_processed_id AND processed_<mode> = 2
  ├─ Load group REPRESENTATIVES for this mode (not all files)
  ├─ Load UNGROUPED processed files (not in any group)
  │
  ├─ STEP 1: Compare new files against representatives ONLY
  │   For each new file:
  │     ├─ Load artifacts (phash/features/embedding)
  │     ├─ Skip if already in a group
  │     ├─ Compare against ALL group representatives
  │     ├─ Select best match (highest similarity >= threshold)
  │     ├─ If match found:
  │     │   ├─ Add to that group
  │     │   ├─ Check if new file should become representative (larger/older)
  │     │   └─ Update representative cache if swapped
  │     └─ If no match: Add to batch ungrouped list
  │
  └─ STEP 2: Create new groups from batch ungrouped files
      For each ungrouped file in batch:
        ├─ Compare against other batch ungrouped files
        ├─ Find all similar files (similarity >= threshold)
        ├─ If 2+ similar files found: Create new group
        └─ Add new group's representative to cache
  │
  └─ STEP 3: Cross-group similarity checking
      ├─ Identify files with similarity < threshold.max in current groups
      ├─ Compare against representatives of other groups
      ├─ Move files to groups with higher similarity scores
      └─ Update group member counts
  │
  └─ Update checkpoint
```

**Key Principles:**

- **Representative-Based:** New files compared ONLY against group representatives, not all members
- **Non-Transitive:** If A~B and B~C but A≁C, they will be in different groups
- **Highest Similarity:** If multiple representatives match, add to group with highest similarity
- **2+ Required:** New groups only created when 2+ similar ungrouped files found in batch
- **Cross-Group Optimization:** Files with low similarity scores are checked against other groups for better matches

### 3. Group Management

**Creating a Group**:

1. Select representative using size/age priority
2. Create group record with representative info
3. Add all members to duplicate_members table
4. Mark representative member with `is_representative = 1`

**Adding to Existing Group**:

1. Load current group representative
2. Compare new file against current representative
3. If new file is better representative:
   - Update old representative flag to 0
   - Update group with new representative
   - Set new representative flag to 1
4. Increment member_count in group
5. Add member to duplicate_members table

## Configuration

### Core Settings

| Key                                | Default   | Description                            |
| ---------------------------------- | --------- | -------------------------------------- |
| `duplicates.finder.enabled`        | `true`    | Enable/disable duplicate detection     |
| `duplicates.finder.intervalMs`     | `3600000` | Run every hour (live update supported) |
| `duplicates.finder.batchSize`      | `1000`    | Process 1000 files per batch           |
| `duplicates.finder.maxGroupSize`   | `100`     | Max duplicates per group               |
| `tpm.types.duplicate_finder.share` | `1.0`     | Thread pool allocation                 |

**Note**: `duplicates.finder.intervalMs` is monitored for config changes and will update the job interval dynamically without requiring a restart. Changes take effect on the next scheduled run.

### Similarity Thresholds (Two-Tier System)

The system uses a **similarity/duplicate threshold** pair for EMBEDDING mode:

| Key                              | Default | Description                                                          |
| -------------------------------- | ------- | -------------------------------------------------------------------- |
| `duplicates.threshold.similarity`| `0.92`  | Minimum to consider for a group; scores in [similarity, duplicate) are "similar" tier |
| `duplicates.threshold.duplicate` | `0.96`  | True duplicate tier; scores >= this are duplicates of the key image |

**Threshold Usage:**

- **Similarity Threshold**: Minimum score vs key to consider a file for a group. Scores in [similarity, duplicate) are the "similar" tier and may trigger split analysis.
- **Duplicate Threshold**: Scores >= this are true duplicates. Only files in this tier can become representatives. Cross-group checking uses this as the low-similarity cutoff.

**Split Logic:** When a new file matches the best group's key in the similar tier, we check if it crosses duplicate threshold with any similar member. If so, we create a new group and redistribute similar members by higher score.

**Reprocessing:** Similarity threshold decrease triggers full reprocessing. Duplicate threshold changes do not.

**Configuration Examples:**

```yaml
# Conservative (High Precision)
duplicates.threshold.similarity: 0.95
duplicates.threshold.duplicate: 0.98

# Balanced (Recommended)
duplicates.threshold.similarity: 0.92
duplicates.threshold.duplicate: 0.96

# Permissive (High Recall)
duplicates.threshold.similarity: 0.88
duplicates.threshold.duplicate: 0.94
```

### Representative Selection

| Key                                  | Default         | Options                            |
| ------------------------------------ | --------------- | ---------------------------------- |
| `duplicates.representative.strategy` | `size_then_age` | `size_then_age` \| `age_then_size` |

## Incremental Processing

The system uses a checkpoint-based approach to avoid reprocessing the entire dataset:

### Checkpoint Tracking

Each server mode (FAST/BALANCED/QUALITY) maintains its own checkpoint:

- `last_processed_id`: Highest scanned_files.id that has been checked
- `last_run_timestamp`: When the last run completed
- Statistics: files_checked, duplicates_found, groups_created, groups_updated

### Benefits

1. **Efficiency**: Only processes new files, not entire dataset
2. **Scalability**: Handles growing datasets without performance degradation
3. **Crash Recovery**: Resumes from last checkpoint after restart
4. **Mode Independence**: Each mode tracks progress independently

## Thread Pool Integration

The duplicate finder is registered as a scheduled job with the ThreadPoolManager:

- **Job ID**: `"duplicateFinder"`
- **Task Type**: `"duplicate_finder"`
- **Thread Share**: Configurable via `tpm.types.duplicate_finder.share`
- **Execution**: Asynchronous, non-blocking
- **Failure Handling**: Scheduler retries with exponential backoff

## Performance Considerations

### Complexity

- **Per-file comparison**: O(G) where G = number of existing groups (representatives only)
- **Per-batch**: O(B × G) where B = batch size, G = number of groups
- **New group creation**: O(U²) where U = ungrouped files in batch
- **Memory**: Loads only representatives + ungrouped files (not all processed files)
- **Scalability**: Excellent - complexity grows with groups, not total files
- **Representative-based**: Dramatically faster than comparing against all files

### Optimization Strategies

1. **Batch Size**: Adjust `duplicates.finder.batchSize` based on dataset size
2. **Interval**: Increase `duplicates.finder.intervalMs` for large datasets
3. **Thresholds**: Higher thresholds = fewer comparisons = faster processing
4. **Thread Share**: Allocate more threads if duplicate detection is priority

### Scalability

For datasets > 100K files:

- Consider lowering batch size (e.g., 500)
- Increase execution interval (e.g., 4 hours)
- Use mode-specific thresholds to reduce false positives

## Cross-Group Similarity Checking

The system includes **cross-group similarity checking** that identifies files with low similarity scores in their current groups and checks if they would be better matched in other groups.

### How It Works

1. **Identification**: After normal duplicate processing, identifies files with similarity scores **below** `threshold.max` in their current groups
2. **Cross-Group Comparison**: These low-similarity files are compared against representatives of **other groups**
3. **File Movement**: If a file finds a better match (higher similarity score) in another group, it's moved there
4. **Group Updates**: Member counts and representative information are updated accordingly

### Example Scenario

```
Group 1: File A (rep), File B (similarity=0.75 to A)
Group 2: File C (rep), File D (similarity=0.85 to C)

If threshold.max = 0.80:
- File B (0.75 < 0.80) is eligible for cross-group checking
- File B compared to File C → similarity = 0.90
- Since 0.90 > 0.75, File B moves to Group 2
- Result: Group 1 has File A only, Group 2 has Files C, D, B
```

### Configuration Impact

**Narrow Range (More Cross-Group Checking):**

```yaml
duplicates.threshold.min: 0.90
duplicates.threshold.max: 0.95 # More files below threshold
```

- More aggressive optimization
- More files checked and moved

**Wide Range (Less Cross-Group Checking):**

```yaml
duplicates.threshold.min: 0.85
duplicates.threshold.max: 0.98 # Fewer files below threshold
```

- More conservative optimization
- Fewer files checked and moved

### Performance Considerations

- Cross-group checking runs **after** normal batch processing
- Only files with low similarity scores are checked
- Representative-based comparison (not all-pairs) for efficiency
- Metadata filtering prevents incompatible file comparisons

## Algorithm Evolution

### v3 - Representative-Based Matching (October 2025 - Current)

**Changes:**

- Compare new files ONLY against group representatives (not all members)
- Select group with highest similarity if multiple matches
- Non-transitive grouping: Files must be similar to representative, not just any member
- New groups created only when 2+ similar ungrouped files found in batch
- Dramatically improved performance: O(B × G) instead of O(B × N)

**Benefits:**

- Correct semantics: Each group driven by its representative file
- Prevents one massive group with all "somewhat similar" files
- Scales better with large datasets (10x-100x faster)
- Representative automatically swaps when larger/older file added

### v2 - Cross-Batch Comparison (October 2025 - deprecated)

**Issues Discovered:**

- Compared against ALL files, not just representatives
- Created one massive group (647 members) due to transitive matching
- Poor performance: O(B × N) where N = all files

### v1 - Original Bugs

**Issues:**

- Groups were always pairs due to batch-only comparison
- Existing files from previous batches were never loaded
- No cross-batch duplicate detection

## Future Enhancements

Potential improvements not yet implemented:

1. **Approximate Nearest Neighbors (ANN)**: Use FAISS/Annoy for embedding similarity
2. **Spatial Hashing**: Bucket files by pHash prefix for faster lookup
3. **Parallel Processing**: Multi-threaded similarity computation
4. **Complete Group Merging**: Automatically merge multiple groups when bridged
5. **User Confirmation**: UI for reviewing/confirming duplicates
6. **Auto-Deletion**: Configurable rules for automatic duplicate removal
7. **Cross-Mode Detection**: Compare FAST+BALANCED+QUALITY results
8. **File Metadata**: Consider EXIF data, camera info in comparison

## API Integration

While not yet implemented, the schema supports future API endpoints:

- `GET /api/duplicates` - List all duplicate groups
- `GET /api/duplicates/{group_id}` - Get group members
- `GET /api/duplicates/file/{file_path}` - Find duplicates of specific file
- `POST /api/duplicates/detect` - Trigger manual detection run
- `DELETE /api/duplicates/{group_id}` - Unlink duplicate group

## Testing

Comprehensive unit tests should cover:

1. **SimilarityCalculator**: All three algorithms with known inputs
2. **DuplicateGroupsOps**: All database operations
3. **DuplicateFinder**: Incremental processing, checkpoint management
4. **Representative Selection**: Size/age priority logic
5. **Integration**: End-to-end duplicate detection workflow

## Monitoring

Key metrics to monitor:

- Groups created per run
- Duplicates found per run
- Processing time per batch
- Checkpoint advancement rate
- Thread pool utilization (duplicate_finder task type)

## Troubleshooting

### Duplicate finder not running

- Check `duplicates.finder.enabled = true`
- Verify scheduler service is running
- Check logs for initialization errors

### No duplicates found

- Verify files are processed (status = 2)
- Check threshold settings (may be too strict)
- Ensure artifacts exist in `image_artifacts` table

### Performance issues

- Reduce `duplicates.finder.batchSize`
- Increase `duplicates.finder.intervalMs`
- Adjust `tpm.types.duplicate_finder.share`
- Check database indexes are created

### Representative not updating

- Verify file_metadata contains size and date
- Check representative selection strategy
- Review logs for group update operations
- Ensure new file has similarity >= `duplicates.threshold.max` to become representative

### Historical Bug: Pairwise Groups (Fixed October 2025)

**Problem:** All duplicate groups contained exactly 2 members instead of grouping all similar files together.

**Root Causes:**

1. **No Cross-Batch Comparison**: Existing files from previous batches were never loaded, so new files could only compare against other files in the same batch
2. **Pairwise Group Creation**: Code created groups with exactly 2 files and immediately broke, preventing larger groups
3. **Zero Thresholds**: Configuration had thresholds set to 0, making every file match every other file

**Solution:**

- Fixed existing file loading to properly load all processed files with artifacts
- Implemented all-pairs similarity check within batches to find all similar files
- Added cross-batch comparison against existing group representatives
- Fixed threshold configuration defaults

**Current Status:** ✅ Fixed - Groups now properly contain all similar files across batches

---

## Image Processing Architecture

This document describes the end-to-end image processing pipeline in the Media Dedup Server, covering control flow, threading, configuration, storage, and the three processing tiers (Fast, Balanced, Quality). It reflects the current implementation status in this repository.

### High-level Goals

- Reliable, asynchronous processing of images discovered by the file scanner
- SAFE TREO principles in the processing path: Timeouts, Retries, Error isolation, Observability
- All artifacts persisted in the database (no on-disk indexes)
- CPU-only stack; external libraries detected via CMake and optional at build time

### Key Components

- Files discovery: `FilesManager` emits candidate files into the database table `scanned_files`. Status codes per mode: -1 error, 0 ready, 1 picked up, 2 done.
- Orchestration: `MediaProcessor` receives unprocessed files by mode and routes to the correct media category (currently images). It uses `ThreadPoolManager` for fire-and-forget lambdas.
- Threading: `ThreadPoolManager` wraps Poco::ThreadPool with config-driven idle timeout and per-task shares. Lambdas submit work for `image_processor` task type.
- SAFE TREO wrapper: `ImageTask` encapsulates a single-file unit of work with timeouts, retries, and structured logging (observability). It executes a chosen pipeline and updates status in `scanned_files`.
- Pipelines: Three tiers
  - Fast: thumbnail + perceptual hash (OpenCV), saved as 64-bit pHash BLOB and thumb size
  - Balanced: local features (OpenCV ORB/SIFT) serialized and saved as BLOB; includes resize and keypoint capping
  - Quality: deep embeddings (CLIP image encoder via ONNX Runtime), saved as float32 BLOB with model metadata
- Storage: `image_artifacts` table holds per-image outputs across tiers.

### Data Model

- `scanned_files` (existing): tracks file metadata and per-mode processing flags; unchanged files are not reset erroneously thanks to strict metadata comparison (size, createdAt, modifiedAt).
- `image_artifacts` (new):
  - file_path (PK), phash BLOB, thumb_w, thumb_h
  - features_method TEXT, features BLOB
  - embedding_model TEXT, embedding_dim INTEGER, embedding BLOB
  - version, created_at, updated_at

### Control Flow

1. Server starts: `ServerInitializer` ensures crash-recovery by clearing only in-progress (1) flags back to 0 once.
2. Scheduler triggers `MediaProcessor::ProcessMedia` periodically (interval cfg). It queries `scanned_files` for ready files (status=0 for the active mode).
3. For each file, `MediaProcessor` submits a lambda to `ThreadPoolManager` with task name `image_processor`.
4. The lambda constructs `ImageTask` specifying TREO config (timeouts, retries), then invokes a tier-specific pipeline:
   - Fast → `FastPipeline::Run`
   - Balanced → `BalancedPipeline::Run`
   - Quality → `QualityPipeline::Run`
5. `ImageTask` updates `scanned_files` to -1 or 2 based on success/failure, with start/finish logging.

### SAFE TREO Details

- Timeouts: `ImageTaskConfig.timeout_ms` controls max per-file processing time. If exceeded, the task is treated as failed (retryable or not based on error type) and the DB is updated.
- Retries: `retry.enabled`, `maxAttempts`, `baseDelayMs` implement limited exponential backoff for transient issues (I/O, temporary ONNX/OpenCV errors). Non-retryable cases (decode/model errors) fail immediately.
- Error isolation: Tasks run as independent lambdas; failures are contained and logged with context. No unhandled exceptions escape to the pool.
- Observability: Structured logs at start/finish with duration, outcome, and artifact stats; warnings for unsupported types or missing models.

### Backends & Pipelines

- OpenCV Backend (`OpenCvAdapter`)

  - Disables internal threading to respect global thread pool concurrency
  - Loads image, resizes with aspect ratio, computes pHash via `cv::img_hash::pHash`
  - Derives a stable 64-bit representation for storage

- Features Backend (`FeaturesAdapter`)

  - Loads image, optional grayscale, resizes moderately
  - Extracts ORB/SIFT keypoints and descriptors; caps top N keypoints (config)
  - Serializes compact representation to BLOB

- ONNX Backend (`OnnxAdapter`)
  - Preprocess: sRGB conversion, resize to model input (e.g., 224×224), normalization
  - Loads an image-encoder ONNX model (CLIP ViT-B/32 or RN50 image encoder)
  - Runs inference (CPU), returns embedding vector (e.g., 512-D float32)
  - Handles missing/invalid model path by failing the task (no downgrade)

### Configuration

Relevant keys (subset):

- Thread pool:

  - `tpm.thread.idleTimeoutSeconds`: idle timeout; triggers pool recreation on change
  - `tpm.types.media_processor.share`: share for media processing tasks (consolidated naming)
  - `tpm.types.image_processor.share`: reserved for future dedicated image processor

- Scheduler:

  - `media.processor.intervalMs`: processing trigger interval

- Image Task (TREO):

  - `media.image.timeoutMs`
  - `media.image.retry.enabled`
  - `media.image.retry.maxAttempts`
  - `media.image.retry.baseDelayMs`

- Fast tier:

  - `media.image.fast.thumbSize`

- Balanced tier:

  - `media.image.balanced.resizeLongEdge`
  - `media.image.balanced.maxKeypoints`

- Quality tier:
  - `media.image.quality.onnx.modelPath`
  - `media.image.quality.onnx.inputSize`
  - `media.image.quality.embeddingDim`

All config lookups are performed via `UnifiedObservableConfigManager`; components subscribe to changes where applicable (e.g., thread pool idle timeout, shares).

### Error Handling & Status Codes

- Status codes in `scanned_files` per mode:
  - -1: permanent failure
  - 0: ready to process
  - 1: picked up (in queue)
  - 2: processed successfully
- Unsupported type or missing model: mark as -1 and log
- DB write errors (e.g., locked): retried by `ImageTask` based on retry policy; final failure sets -1.

### Build & Dependencies

- CMake detects OpenCV and ONNX Runtime; compilation guards:
  - `HAVE_OPENCV`, `HAVE_ONNXRUNTIME`
- ONNX headers are vendored automatically if not found (FetchContent)
- Example app `quality_smoke` validates Quality path end-to-end

### Extension Points

- New pipelines (e.g., denoise) can follow the `Run(file, cfg, db)` pattern and be wired in `MediaProcessor` routing.
- Additional artifact types can extend `image_artifacts` table (e.g., color histograms) with corresponding upsert ops.
- Future duplicate detection will consume artifacts to compute similarity (out of scope for current phase).

### Current Status

- Fast, Balanced, Quality scaffolds are implemented; Fast and Quality paths write artifacts to DB. Quality uses a known-good CLIP image encoder ONNX model.
- Unit tests cover DB ops and processor routing; `quality_smoke` example verifies an embedding row is created.

---

# Persisted Vector Storage in SQLite (sqlite-vec)

Since we can start the DB from scratch, we can add vector search **inside the same SQLite database** using the **sqlite-vec** extension. Storage is persisted in the same `.sqlite` file.

---

## 1. What is sqlite-vec?

- **SQLite extension** (loadable `.so` / `.dylib` / `.dll`) that adds vector types and KNN search.
- **vec0 virtual tables**: store vectors in a virtual table; query with `WHERE embedding MATCH ? AND k = N` for K-nearest neighbors.
- **Persistence**: vec0 tables live in your existing SQLite database file; no separate store.
- **Distance**: Supports **cosine** (we use cosine similarity), L2, L1, so we can use `distance_metric=cosine` and keep our thresholds.
- **Pure C, no extra runtime deps**; works on macOS, Linux, Windows.

---

## 2. What we need to add

### A. Dependency: sqlite-vec extension

- **Option 1 – Prebuilt:** Download the extension for your platform from [sqlite-vec releases](https://github.com/asg017/sqlite-vec/releases) (e.g. `vec0.dylib` for macOS, `vec0.so` for Linux). Put it in a known path (e.g. `dedup_server/extensions/` or next to the binary).
- **Option 2 – Build from source:** Clone [asg017/sqlite-vec](https://github.com/asg017/sqlite-vec), build the shared library, install it where the server can load it.

The app will **load** this extension at DB open time; the DB file itself stays a normal SQLite DB.

### B. Load the extension when the DB is opened

SQLite must load the extension before creating or using vec0 tables. With **Poco::Data::SQLite** we open the DB via `Session("SQLite", db_path)`. To load an extension you need the underlying `sqlite3*` handle:

- **Preferred:** If Poco exposes the native connection (e.g. via a backend accessor), use it to call `sqlite3_load_extension(db, "path/to/vec0", nullptr, nullptr)` once per connection (or once per process if connections share the same DB).
- **Alternative:** Run `SELECT load_extension('path/to/vec0');` as SQL. Many SQLite builds disable `load_extension()` by default; in that case we must either enable it at compile time or use the C API from code that has the `sqlite3*` handle.

So the **code change** is: in `DatabaseManager::initialize()` (or in the session manager when a new connection is created), after the connection is open, load the extension. That may require adding a small path that uses the native SQLite C API for that single call if Poco doesn’t expose it.

### C. Create a vec0 virtual table for embeddings

We already have embeddings in `image_artifacts` (BLOB, float32, `embedding_dim` e.g. 512). We have two design options:

**Option 1 – Single “index” table (recommended for start):**

- One vec0 table that holds **all processed file embeddings** we care about for duplicate search (e.g. group representatives + ungrouped, or simply all with `processed = 2`).
- Schema idea:
  - `rowid` or primary key = `file_id` (so we can JOIN back to `scanned_files` / `image_artifacts`).
  - One vector column, e.g. `embedding float[512]` (match `embedding_dim` from config), with `distance_metric=cosine`.
- Example (dim and metric from config):

```sql
CREATE VIRTUAL TABLE IF NOT EXISTS vec_embedding_index USING vec0(
  file_id INTEGER PRIMARY KEY,
  embedding float[512] distance_metric=cosine
);
```

- **Persistence:** This table is part of the same SQLite DB; it’s stored in the same file as `scanned_files`, `image_artifacts`, `duplicate_groups`, etc.

**Option 2 – Separate tables for “reps” vs “ungrouped”:**

- e.g. `vec_rep_embeddings` and `vec_ungrouped_embeddings` with the same vector shape. Duplicate finder would run two KNN queries (reps, then ungrouped). More flexible for filtering; a bit more sync logic.

For a first version, Option 1 is enough: one index over “all embeddings that participate in duplicate search,” and we filter by `file_id` in `duplicate_groups` / `duplicate_members` when interpreting results.

### D. Keep the vector table in sync (single source of truth)

- **On artifact write:** When we insert/update a row in `image_artifacts` (embedding computed), we also **INSERT OR REPLACE** into the vec0 table: `(file_id, embedding)`. So the vec table is a **mirror** of the embeddings we want to search.
- **On group changes:** When we create/delete groups or change the representative, we do **not** need to change the vec table if we index “all processed embeddings” and resolve rep vs ungrouped in app logic. If we had separate “reps only” tables we’d remove/add rows when reps change.
- **Dimension:** `embedding_dim` is config-driven (e.g. 512). The vec0 table must use the same dimension; we can create it at startup from config (or recreate it if the dimension ever changes and we’re OK to rebuild).

So the **changes** are:

- Where we write to `image_artifacts`, also write the same `(file_id, embedding)` into the vec0 table.
- Optionally: when we delete or overwrite an artifact, delete or replace the corresponding row in the vec table so the index doesn’t keep stale vectors.

### E. Duplicate finder: use KNN instead of full scan

**Today:** For each new file we load all group representatives (and ungrouped) and call `computeSimilarity` in a loop.

**With vec0:**

1. Load the new file’s embedding (we already have it).
2. Run one (or two) KNN query(ies), e.g.:
   - `SELECT file_id, distance FROM vec_embedding_index WHERE embedding MATCH :embedding_blob AND k = 100`
   - Bind the new file’s embedding as `:embedding_blob`.
3. sqlite-vec returns rows ordered by **distance** (for cosine, smaller = more similar). Convert to “similarity” if needed (e.g. `similarity = 1 - distance` for cosine distance, depending on how sqlite-vec defines it).
4. Take the top candidates (e.g. top 50 or 100), then in code:
   - Resolve which of those `file_id`s are current group representatives vs ungrouped (JOIN to `duplicate_groups` / `duplicate_members` or use a small query).
   - Apply the **same** logic as today: best match above similarity threshold, duplicate vs similar tier, split, add to group, cross-batch style.

So the **only** behavioral change in the finder is **how we get the candidate set** (KNN from vec0 instead of “load everyone and loop”). Thresholds, two-tier logic, and split/redistribution stay the same; we just run them on the KNN result set.

### F. Schema / migrations

- Since we’re starting from scratch, we can:
  - Add “create vec0 table” to the same place we create other tables (e.g. in `ensureTables` or a dedicated “ensure vector index” step that runs after the extension is loaded).
  - Use `CREATE VIRTUAL TABLE IF NOT EXISTS vec_embedding_index USING vec0(...)` so it’s idempotent.
- We do **not** need to migrate old `image_artifacts` BLOBs into vec0 if we’re starting fresh: the first time we write an artifact we also write to vec0, and we can backfill vec0 from existing `image_artifacts` once at startup if we ever add the extension to an existing DB later.

---

## 3. Summary: what is needed

| Item | What to do |
|------|------------|
| **Persistence** | Use sqlite-vec **inside** the same SQLite DB; vec0 virtual table is stored in the same `.sqlite` file. No separate vector DB. |
| **Dependency** | Add the sqlite-vec loadable extension (prebuilt or built from source) and a known path for it. |
| **Load extension** | After opening the DB (in `DatabaseManager` or session manager), load the extension via `sqlite3_load_extension()` or `SELECT load_extension(...)` so vec0 is available. |
| **Create table** | Run `CREATE VIRTUAL TABLE vec_embedding_index USING vec0(file_id, embedding float[N] distance_metric=cosine)` with N = `embedding_dim`, in the same place we create other tables. |
| **Sync** | On every insert/update of an embedding in `image_artifacts`, insert/replace the same (file_id, embedding) into `vec_embedding_index`. |
| **Duplicate finder** | Replace the “load all reps / ungrouped and loop” step with “query vec_embedding_index WHERE embedding MATCH :new_embedding AND k = K”, then run existing threshold/split logic on the returned file_ids. |

That’s what’s needed to have **persisted vector storage as part of SQLite itself** and to use it for duplicate search without worrying about existing data, since we’re starting from scratch.

---

## 4. Configuration and extension path

The server uses the following config keys (see `config_manager_factory.cpp` and `config/config.yaml`):

| Key | Default | Description |
|-----|---------|-------------|
| `database.vector.enabled` | `false` | Set to `true` to enable the vector index (requires extension path). |
| `database.vector.extensionPath` | `""` | Full path to the sqlite-vec loadable extension (e.g. `vec0.dylib` on macOS, `vec0.so` on Linux). |
| `duplicates.finder.knnK` | `200` | When the vector index is enabled, only this many KNN candidates are considered per file; `0` disables KNN and uses full scan. |

**Obtaining the extension**

- **Prebuilt:** Download for your platform from [sqlite-vec releases](https://github.com/asg017/sqlite-vec/releases) (e.g. `vec0-0.1.0-x86_64-apple-darwin.zip` → `vec0.dylib`). Place the shared library in a fixed path and set `database.vector.extensionPath` to that path (e.g. `/opt/media-dedup/extensions/vec0.dylib` or relative to the server binary).
- **Build from source:** Clone [asg017/sqlite-vec](https://github.com/asg017/sqlite-vec), build the shared library (`make` or CMake), then set `database.vector.extensionPath` to the built `vec0.dylib` / `vec0.so`.

The server opens a **dedicated** SQLite connection (separate from Poco sessions) to load the extension and create/query the vec0 table; the main DB file path is the same as the rest of the app. If the extension fails to load or the vec0 table cannot be created, the vector index is left disabled and the duplicate finder falls back to the full linear scan over representatives and ungrouped files.

---

## Object-Aware Template Pipeline

### Intent

- Extend media processing so every image produces region-level embeddings (general, face, nature/building) plus a fused template vector for duplicate detection.
- Preserve current API contracts while replacing the underlying artifact storage and similarity math.
- Enable future region search workflows (lasso queries, named regions) by making region vectors instantly searchable through ANN indexes.

---

### Processing Flow Changes

1. **Decode + Preprocess**
   - Reuse existing loader; ensure decoded frame is handed to all downstream extractors to avoid redundant IO.
2. **Detectors**
   - General objects: YOLOv8-seg (pretrained) on GPU; outputs bounding boxes + masks.
   - Faces: RetinaFace for bounding boxes and landmarks.
   - Nature/buildings: leverage YOLO labels + Places365 scene classifier to tag mountains, skyline, trees.
3. **Embedding Generation**
   - General encoder: CLIP ViT-B/32 ONNX model; run on full frame and each detected region (crops or masked).
   - Face encoder: ArcFace ResNet100 ONNX model, normalized outputs.
   - Nature/building encoder: ViT trained on Places365; only invoked for regions with matching labels.
4. **Region Records**
   - Assign `region_id` per detection, capture label, detector_type, confidence, bbox, optional mask reference.
   - Store general embedding for **every** region; attach specialized embedding where applicable (face, nature/building).
5. **Unified Template**
   - Inputs: whole-image CLIP vector + pooled (mean + max) general region vectors + pooled specialized vectors.
   - Pass through lightweight projection MLP (e.g., [dim 512 → 1024 → 512]) to create final template vector used by duplicate detection.
6. **Indexing**
   - Insert region embeddings into FAISS indexes (separate index per model_type) during processing.
   - Record index metadata (version, vector count) to support rebuilds.

---

### Database Schema (fresh DB)

#### `image_templates`

| Column                      | Type      | Notes                              |
| --------------------------- | --------- | ---------------------------------- |
| `image_id`                  | FK        | References existing `images` table |
| `template_version`          | TEXT      | e.g., `templates.v2`               |
| `vector`                    | BLOB      | 512-d float array (serialized)     |
| `created_at` / `updated_at` | TIMESTAMP | managed by ORM                     |

#### `image_regions`

| Column                                 | Type          | Notes                                          |
| -------------------------------------- | ------------- | ---------------------------------------------- |
| `region_id`                            | INTEGER PK    | Unique per region                              |
| `image_id`                             | FK            | Parent image                                   |
| `label`                                | TEXT          | e.g., `face`, `mountain`, `building`, `person` |
| `detector_type`                        | TEXT          | `yolov8`, `retinaface`, `places365`            |
| `confidence`                           | REAL          | 0–1                                            |
| `bbox_x`, `bbox_y`, `bbox_w`, `bbox_h` | REAL          | Normalized coordinates                         |
| `mask_path`                            | TEXT nullable | Optional serialized mask                       |
| `general_embedding_id`                 | FK            | Points to `region_embeddings`                  |
| `special_embedding_id`                 | FK nullable   | Points to specialized embedding (face/nature)  |
| `created_at`                           | TIMESTAMP     |                                                |

#### `region_embeddings`

| Column          | Type       | Notes                                |
| --------------- | ---------- | ------------------------------------ |
| `embedding_id`  | INTEGER PK |                                      |
| `region_id`     | FK         |                                      |
| `model_type`    | TEXT       | `general`, `face`, `nature_building` |
| `model_version` | TEXT       | e.g., `clip.vitb32.onnx.v1`          |
| `dimension`     | INTEGER    | 512 / 256 etc.                       |
| `vector`        | BLOB       | Raw float bytes                      |
| `created_at`    | TIMESTAMP  |                                      |

#### `region_index_metadata`

Tracks FAISS indexes.

| Column          | Type      | Notes                             |
| --------------- | --------- | --------------------------------- |
| `model_type`    | TEXT PK   |                                   |
| `model_version` | TEXT PK   |                                   |
| `index_uuid`    | TEXT      | Identifier for stored FAISS index |
| `index_type`    | TEXT      | e.g., `IVF_PQ`, `HNSW32`          |
| `vector_count`  | INTEGER   |                                   |
| `built_at`      | TIMESTAMP |                                   |

---

### Duplicate Detection Adjustments

- Loader pulls unified vectors from `image_templates` instead of legacy artifact blobs.
- Configuration adds `duplicates.templates.v2.threshold_min/max`; tuning happens separately once embeddings are validated.
- Representative cache stores `(image_id, template_version, vector)`; mismatched versions trigger regeneration or fallback.
- Similarity calculator remains cosine-based but must accept new vector length (512) and normalizes accordingly.
- Batch flow (compare to representatives → add/move members → update checkpoint) stays unchanged, so API responses do not change.

---

### API Impact

- `/api/duplicates/*` endpoints continue emitting the same fields (`group_id`, `representative_path`, similarity values).
- Internal DTOs source template vectors via new DAO; no response contract changes.
- Logging/metrics add template/index version tags for observability.

---

### Region Search Enablement

- Lasso query flow: crop → CLIP embedding → search `general` FAISS index; if classifier says “face” or “nature/building,” also search specialized indexes.
- Named regions map to stored region IDs; label metadata stays alongside the embedding so we can filter (e.g., only faces).
- Future API work can expose `/api/regions/search` without modifying duplicate detection endpoints.

---

### Open Tasks (Future PRs)

1. Implement detector + embedding orchestrator (reuse existing media processor hooks).
2. Define serialization helpers for embeddings (float32 → BLOB) and integrate with database manager.
3. Train/validate fusion MLP and set threshold defaults.
4. Build FAISS management utilities (insert, snapshot, rebuild).
5. Add migration scripts to create the new tables and drop legacy artifact columns during fresh deploy.

---

# Scan Path Change API - Detailed Design

## Overview

This document describes the design for an API endpoint that allows changing registered scan paths. When a media location is moved (e.g., external drive remounted at different path), this endpoint verifies the new path and updates all associated file paths in the database.

## Design Goals

1. **Path Change API**: Single endpoint to change a registered scan path
2. **Verification**: Random sampling of files to verify new path works
3. **Atomic Update**: Update all database references atomically
4. **Comprehensive Updates**: Update all tables that reference file paths

## Architecture Components

### 1. API Endpoint

**Endpoint**: `POST /api/v1/media-locations/change-path`

**Request Body**:
```json
{
  "old_path": "/Volumes/OldDrive/Photos",
  "new_path": "/Volumes/NewDrive/Photos",
  "sample_size": 10  // Optional: number of files to verify (default: 20)
}
```

**Response**:
```json
{
  "status": "ok",
  "old_path": "/Volumes/OldDrive/Photos",
  "new_path": "/Volumes/NewDrive/Photos",
  "files_verified": 20,
  "files_verified_success": 20,
  "files_updated": 1500,
  "verification_success_rate": 1.0
}
```

### 2. Database Tables Requiring Updates

The following tables reference file paths and need updates:

1. **scanned_files**:
   - `file_path` (PRIMARY KEY) - Replace prefix
   - `location_key` - Update to new location key

2. **image_artifacts**:
   - `file_path` (PRIMARY KEY) - Replace prefix

3. **processing_errors**:
   - `file_path` - Replace prefix

4. **duplicate_groups**:
   - `representative_file_path` - Replace prefix

5. **duplicate_members**:
   - `file_path` - Replace prefix

6. **thumbnail_cache**:
   - `source_path` - Replace prefix

7. **user_settings**:
   - Update old `mediaLocation:<hash>` entry to new path value
   - Optionally create new entry and delete old (if keeping old key)

### 3. File Association Strategy

Files are associated with a scan path in two ways:

1. **By location_key**: All files where `location_key` matches the old location key
2. **By file_path prefix**: All files where `file_path` starts with the old path

**Decision**: Use **location_key** as primary association method because:
- More reliable (doesn't depend on path structure)
- Already indexed
- Consistent with how files are registered during scanning

### 4. Path Update Algorithm

For each file associated with old location:

1. **Extract relative path**: `relative_path = file_path - old_path`
2. **Construct new path**: `new_file_path = new_path + relative_path`
3. **Update all references**: Update file_path in all tables

**Example**:
- Old path: `/Volumes/OldDrive/Photos`
- File: `/Volumes/OldDrive/Photos/2024/image.jpg`
- Relative: `2024/image.jpg`
- New path: `/Volumes/NewDrive/Photos`
- New file: `/Volumes/NewDrive/Photos/2024/image.jpg`

### 5. Verification Process

1. **Query files**: Get all files with matching `location_key`
2. **Random sampling**: Select random sample (default: 20 files, configurable)
3. **Verify each file**:
   - Construct new path using relative_path
   - Check if file exists at new path
   - Verify file size matches (optional but recommended)
4. **Success criteria**: 
   - Minimum success rate: 80% (configurable)
   - If verification fails, abort operation and return error

### 6. Implementation Details

#### New Database Methods

**Location**: `include/database/scanned_files_ops.hpp`, `src/database/scanned_files/scanned_files_ops.cpp`

```cpp
// Get all files for a location_key
static std::vector<ScannedFileRow> getFilesByLocationKey(
    DatabaseManager &db, 
    const std::string &location_key
);

// Update file path in scanned_files (and cascade to other tables)
static bool updateFilePath(
    DatabaseManager &db,
    const std::string &old_path,
    const std::string &new_path
);

// Get all files for a location_key
static std::vector<ScannedFileRow> getFilesByLocationKey(
    DatabaseManager &db, 
    const std::string &location_key
);

// Batch update file paths for a location
static int updateFilePathsForLocation(
    DatabaseManager &db,
    const std::string &old_location_key,
    const std::string &old_path,
    const std::string &new_path,
    const std::string &new_location_key
);

// Update file path in other tables (image_artifacts, processing_errors, etc.)
static int updateFilePathInTable(
    DatabaseManager &db,
    const std::string &table_name,
    const std::string &old_path,
    const std::string &new_path
);
```

#### New Service Method

**Location**: `include/filesmanager/files_service.hpp`, `src/filesmanager/files_service.cpp`

```cpp
struct ChangePathResult {
    bool success;
    bool partial_success;  // true if some updates succeeded but not all
    int files_verified;
    int files_verified_success;
    int total_files;
    int files_updated;
    int files_failed;
    double verification_success_rate;
    std::map<std::string, std::pair<int, int>> update_details;  // table_name -> (updated, failed)
    std::string error_message;
};

ChangePathResult changeMediaLocationPath(
    const std::string &old_path,
    const std::string &new_path,
    int sample_size = 20
);
```

#### HTTP Handler

**Location**: `include/core/web/web_handlers_user_settings.hpp`, `src/core/webserver/web_handlers_user_settings.cpp`

```cpp
class ChangeMediaLocationPathHandler : public ConfigRequestHandler {
public:
    ChangeMediaLocationPathHandler(
        std::shared_ptr<UnifiedObservableConfigManager> config_manager,
        std::shared_ptr<FilesService> service
    );
    void handleRequest(
        Poco::Net::HTTPServerRequest &request,
        Poco::Net::HTTPServerResponse &response
    ) override;
private:
    std::shared_ptr<FilesService> service_;
};
```

### 7. Transaction Safety

**Note**: Since we continue on partial failure, we use per-table transactions rather than one large transaction:

1. **Verification phase** (read-only, no transaction needed)
2. **If verification succeeds**:
   - Begin transaction for user_settings updates
   - Create new location_key entry
   - Delete old location_key entry
   - Commit
   - For each table:
     - Begin transaction
     - Batch update file paths
     - Commit (even if some fail, continue with next table)
3. **If verification fails**:
   - Return error response immediately (no database changes)

### 8. Error Handling

**Error Scenarios**:

1. **Old path not registered**: Return 404
2. **New path doesn't exist**: Return 400
3. **Verification failure**: Return 400 with details
4. **Database update failure**: Rollback and return 500
5. **No files found for location**: Return 404

**Error Response Format**:
```json
{
  "status": "error",
  "error": "Verification failed",
  "details": "Only 15 out of 20 files verified successfully (75% < 80% threshold)"
}
```

### 9. OpenAPI Specification

Add to `src/core/webserver/static/api/openapi.json`:

```json
"/api/v1/media-locations/change-path": {
  "post": {
    "summary": "Change a registered media location path",
    "description": "Verifies the new path by sampling files and updates all associated file paths in the database",
    "requestBody": {
      "required": true,
      "content": {
        "application/json": {
          "schema": {
            "type": "object",
            "properties": {
              "old_path": {
                "type": "string",
                "description": "Current registered scan path"
              },
              "new_path": {
                "type": "string",
                "description": "New scan path to change to"
              },
              "sample_size": {
                "type": "integer",
                "description": "Number of files to verify (default: 20)",
                "minimum": 1,
                "maximum": 100,
                "default": 20
              }
            },
            "required": ["old_path", "new_path"]
          }
        }
      }
    },
    "responses": {
      "200": {
        "description": "Path changed successfully",
        "content": {
          "application/json": {
            "schema": {
              "type": "object",
              "properties": {
                "status": {"type": "string", "enum": ["ok", "partial_success", "error"]},
                "old_path": {"type": "string"},
                "new_path": {"type": "string"},
                "files_verified": {"type": "integer"},
                "files_verified_success": {"type": "integer"},
                "total_files": {"type": "integer"},
                "files_updated": {"type": "integer"},
                "files_failed": {"type": "integer"},
                "verification_success_rate": {"type": "number"},
                "update_details": {
                  "type": "object",
                  "additionalProperties": {
                    "type": "object",
                    "properties": {
                      "updated": {"type": "integer"},
                      "failed": {"type": "integer"}
                    }
                  }
                },
                "error": {"type": "string"}
              }
            }
          }
        }
      },
      "400": {
        "description": "Invalid request or verification failed"
      },
      "404": {
        "description": "Old path not registered or no files found"
      },
      "500": {
        "description": "Database update failed"
      }
    }
  }
}
```

## Implementation Plan

### Phase 1: Database Layer
1. Add `getFilesByLocationKey()` method
2. Add `updateFilePath()` method for scanned_files
3. Add batch update methods for other tables
4. Add transaction support

### Phase 2: Service Layer
1. Add `changeMediaLocationPath()` method to FilesService
2. Implement file verification logic
3. Implement path update logic

### Phase 3: HTTP Handler
1. Create `ChangeMediaLocationPathHandler`
2. Register route in `web_server_core.cpp`
3. Add OpenAPI specification

### Phase 4: Testing
1. Unit tests for database methods
2. Unit tests for service method
3. Unit tests for HTTP handler
4. Integration tests

## Finalized Design Decisions

1. **Verification Success Threshold**: **80%** - At least 80% of sampled files must verify successfully
2. **Sample Size**: **20 files** - Random sample of 20 files for verification
3. **File Size Verification**: **Yes** - Verify file exists AND file size matches
4. **Location Key Strategy**: 
   - Since `location_key` is generated from path (SHA1 hash of normalized path), changing the path requires a new location_key
   - **Strategy**: Create new `mediaLocation:<new_hash>` entry in user_settings
   - Update all `scanned_files.location_key` from old key to new key
   - Delete old `mediaLocation:<old_hash>` entry from user_settings
5. **Relative Path Handling**: 
   - `relative_path` is stored but not used for asset access (assets accessed via full `file_path`)
   - **Strategy**: Reconstruct `relative_path` for each file using `lexically_relative` from new root path
   - If reconstruction fails, leave `relative_path` empty (same as during initial scan)
6. **Concurrent Access**: **No locking required** - POST operation, failures are acceptable
7. **Rollback Strategy**: **Continue on partial failure** - Update as many files as possible, report failures in response

## Implementation Details

### Location Key Update Process

1. **Generate new location_key**: `FilesService::makeMediaLocationKey(new_path)`
2. **Create new user_settings entry**: Insert `mediaLocation:<new_hash>` with value `new_path`
3. **Update all scanned_files.location_key**: Change from old key to new key
4. **Delete old user_settings entry**: Remove `mediaLocation:<old_hash>`

### Path Reconstruction Algorithm

For each file:
1. **Extract relative portion**: 
   - If `relative_path` exists and is valid: use it
   - Otherwise: compute `relative_path = file_path - old_path` using path manipulation
2. **Construct new file_path**: `new_file_path = new_path / relative_path`
3. **Reconstruct relative_path**: `new_relative_path = lexically_relative(new_file_path, new_path)`
4. **Update all tables**: Update `file_path` and `relative_path` in scanned_files, and `file_path`/`source_path` in other tables

### Verification Process Details

1. **Query files**: Get all files with `location_key = old_location_key`
2. **Random sampling**: Select 20 random files (or all if < 20)
3. **For each sampled file**:
   - Construct new path: `new_path / relative_path` (or reconstruct if needed)
   - Check file exists: `std::filesystem::exists(new_file_path)`
   - Get file size: `std::filesystem::file_size(new_file_path)`
   - Compare with stored size from `file_metadata` JSON
   - Record success/failure
4. **Calculate success rate**: `success_count / total_sampled`
5. **Decision**: If success_rate >= 0.80, proceed with updates; otherwise abort

### Error Handling

**Partial Update Handling**:
- If some file updates fail, continue with remaining files
- Track success/failure counts per table
- Return detailed statistics in response:
  ```json
  {
    "status": "partial_success",
    "files_updated": 1450,
    "files_failed": 50,
    "details": {
      "scanned_files": {"updated": 1450, "failed": 50},
      "image_artifacts": {"updated": 1200, "failed": 0},
      "duplicate_groups": {"updated": 50, "failed": 0}
    }
  }
  ```

---

### FilesManager Technical Specification

Pre-req: Adhere to Project Operating Rules in `docs/CURSOR_RULES.md`. If this spec and cursor_rules.md ever conflict, cursor_rules.md wins.

## 1) Goal & Scope

- Implement a scheduler-driven FilesManager that periodically scans registered media locations and synchronizes the `scanned_files` table.
- Integrate with existing components: `FilesService` (media locations), `FileScanner` (enumeration), `ScannedFilesOps` (DB ops), `UnifiedObservableConfigManager` (config), and `ThreadPoolManager` (TPM) for scheduling capacity and graceful shutdown.
- Non-goals: content hashing, MIME sniffing, EXIF, duplicate detection (handled later by processing pipelines).

## 2) Placement & Build

- Location: `src/orchestration/files_manager.cpp`, header under `include/orchestration/files_manager.hpp`.
- SchedulerService: `src/orchestration/scheduler_service.cpp`, header under `include/orchestration/scheduler_service.hpp`.
- Unit tests: `tests/unit/test_files_manager.cpp` (added to `all_unit_tests`).

## 3) Responsibilities

- Discover directories to scan from `FilesService::listMediaLocations()` (backed by `user_settings` with `mediaLocation:` keys).
- Load current `scanned_files` rows into an in-memory map keyed by absolute `file_path` for O(1) lookup during scans.
- Traverse each directory via `MediaDedup::Files::scan()` using current scanner options.
- For each emitted `FileRecord`:
  - On per-entry error: log at `warn` and persist brief error fields; continue. Root-level errors: log `warn` only (no persistence).
  - On success: upsert into `scanned_files` with metadata. If changed, reset processing flags to 0.
- Ensure only one scan run is active at a time; skip scheduling if a run is still in progress.
- Shutdown cleanly on Ctrl+C/exit (drain via TPM with configured kill timeout).

## 4) Public API (FilesManager)

```
namespace MediaDedup::Orchestration {
  class FilesManager {
  public:
    FilesManager(std::shared_ptr<UnifiedObservableConfigManager> config,
                 std::shared_ptr<DatabaseManager> db,
                 std::shared_ptr<ThreadPoolManager> tpm,
                 std::shared_ptr<FilesService> filesService);

    void initialize();                  // subscribe to config, ensure tables, prepare internal state
    // Performs a full pass across all locations; SchedulerService will call this on interval
    void runOnce();
    // Optional immediate trigger (returns once the run is queued or skipped if active)
    void triggerScanNow();

  private:
    // internal helpers (no scheduling logic here)
  };
}
```

### 4a) SchedulerService Public API

```
namespace MediaDedup::Orchestration {
  class SchedulerService {
  public:
    SchedulerService(std::shared_ptr<UnifiedObservableConfigManager> config,
                     std::shared_ptr<ThreadPoolManager> tpm);

    void start();
    void stop();

    // Register a periodic job; supports dynamic interval updates via config subscription per job
    void registerJob(const std::string& jobId,
                     std::chrono::milliseconds interval,
                     const std::string& typeKey,
                     std::function<void()> callback);

    void unregisterJob(const std::string& jobId);
  };
}
```

## 5) Configuration (observable)

- New keys (observable; defaults shown):
  - `files.manager.scan.intervalMs` (int, default: 300000) – interval between full scans in milliseconds.
  - `files.manager.enabled` (bool, default: true) – allow disabling scheduling without rebuilding.
  - `tpm.types.fileScan.share` (int; allowed values: 0 or 1; default: 1) – per-type share for the FilesManager task. 0 disables dispatch; 1 enables with full share (no fractional shares for this type).
  - SchedulerService (global defaults; per-job overrides optional under `scheduler.jobs.<jobId>.*`):
    - `scheduler.jitter.enabled` (bool, default: false)
    - `scheduler.jitter.percent` (int 0..100, default: 0)
    - `scheduler.backoff.enabled` (bool, default: true)
    - `scheduler.backoff.initialMs` (int, default: 1000)
    - `scheduler.backoff.maxMs` (int, default: 30000)
    - `scheduler.backoff.multiplier` (double, default: 2.0)
    - `scheduler.backoff.jitterPercent` (int 0..100, default: 10)
    - `scheduler.drift.mode` (string: `anchored` | `fixedDelay`; default: `anchored`)
    - `scheduler.drift.maxDriftMs` (int, default: 60000)
    - Per-job overrides (all optional):
      - `scheduler.jobs.<jobId>.jitter.enabled`
      - `scheduler.jobs.<jobId>.jitter.percent`
      - `scheduler.jobs.<jobId>.backoff.enabled`
      - `scheduler.jobs.<jobId>.backoff.initialMs`
      - `scheduler.jobs.<jobId>.backoff.maxMs`
      - `scheduler.jobs.<jobId>.backoff.multiplier`
      - `scheduler.jobs.<jobId>.backoff.jitterPercent`
      - `scheduler.jobs.<jobId>.drift.mode`
      - `scheduler.jobs.<jobId>.drift.maxDriftMs`
- Uses existing `FileScanner` keys (already implemented):
  - `filesservice.scan.recursive` (bool, default: true)
  - `filesservice.scan.followSymlinks` (bool, default: false)
  - `filesservice.scan.includeHidden` (bool, default: true)
  - `filesservice.log.level` (string: trace|debug|info|warn|error; default: trace)
- Behavior on config changes:
  - Interval or enabled toggles are applied for subsequent scheduling cycles.
  - Scanner options are read at the start of each run.
  - SchedulerService observes its config keys. Changes to jitter/backoff/drift and per-job overrides take effect on subsequent schedules for that job.

## 6) Data Model Mapping (FileRecord -> scanned_files)

- For each file:
  - `file_path` = absolute path from `FileRecord.fullPath` (native separators)
  - `relative_path` = path relative to the scanned root (when determinable)
  - `share_name` = best-effort network share or volume name (if available)
  - `file_name` = basename
  - `file_metadata` (JSON):
    ```json
    {
      "sizeBytes": <uint64>,
      "modifiedAtEpochMs": <int64>,
      "deviceId": "...",
      "inode": "...",
      "symlinkTarget": "...",
      "isHidden": true|false
    }
    ```
  - `is_network_file` = best-effort flag based on platform detection (optional; default false until implemented)
  - `last_seen_at` = TIMESTAMP updated on each successful scan emit (CURRENT_TIMESTAMP on insert/update)
  - Brief error persistence fields:
    - `last_error_code` TEXT NULL (e.g., DIR_NOT_FOUND, PERMISSION_DENIED)
    - `last_error_message` TEXT NULL (brief message)
    - `last_error_errno` INTEGER NULL
    - `last_error_at` TIMESTAMP NULL
    - On a successful emit for the same `file_path` in a later run, clear `last_error_*` and update `last_seen_at`.
- Processing flags:
  - On insert: `processed_fast = processed_balanced = processed_quality = 0` (unprocessed)
  - On detected change: reset same flags to `0` to signal reprocessing is needed.

## 7) Change Detection

- In-memory lookup by `file_path` (unordered_map).
- Compare new `FileRecord` against previously persisted metadata:
  - If identity (deviceId/inode) differs → treat as changed.
  - Else if size or modified time differs → treat as changed.
  - Else if attributes-only differ → optional (currently treated as unchanged for processing).
- On changed: upsert row and reset processed flags to `0`.
- On new: insert row with processed flags set to `0`.

## 8) Scheduling & Concurrency

- FilesManager maintains an atomic `isRunning_` guard to prevent overlapping runs.
- Scheduling is delegated to a reusable `SchedulerService` under `orchestration` with a generic registration API:
  - `register(jobId, intervalMs, typeKey, callback)` (supports dynamic interval updates via config subscription per job)
  - `unregister(jobId)`
  - Internally uses `ThreadPoolManager` to dispatch via `typeKey`
- FilesManager auto-registers on server startup:
  - `jobId = "fileScan"`, `intervalMs = files.manager.scan.intervalMs`, `typeKey = "fileScan"`, `callback = FilesManager::runOnce`
- `tpm.types.fileScan.share` is constrained to 0 or 1 for this task.
- If a run is still active when the next interval elapses, `SchedulerService` skips scheduling (no queue buildup).
- On shutdown, `SchedulerService` stops triggering and allows the in-flight run to finish within `tpm.killTimeoutMs`.

Scheduling semantics:

- Jitter: When enabled, each schedule computes a random offset within ±`jitter.percent` of the interval (global or job override). Jitter is applied per run to avoid synchronized starts.
- Backoff: On callback failure (exception thrown), the next attempt uses exponential backoff based on `initialMs`, `multiplier`, and capped at `maxMs`, with optional backoff jitter. On success, backoff state resets for that job.
- Drift control: In `anchored` mode, the cadence is anchored to the nominal schedule (e.g., every N ms), and actual start times are adjusted to not exceed `drift.maxDriftMs` from the anchor. In `fixedDelay` mode, the next run is scheduled relative to the completion time. Configurable per job via overrides; defaults are global.

## 9) Thread Safety

- In-memory index guarded by `std::mutex` or `std::shared_mutex` (read-heavy pattern).
- All DB operations obtain sessions via `DatabaseManager::acquireSessionLease()` (RAII, pooled, timeout/backoff per config).
- `FilesService` and `UnifiedObservableConfigManager` accessed through shared pointers; callbacks unregistered on stop.

## 10) Logging

- Use project logging levels (trace, debug, info, warn, error):
  - trace: run start/stop, directory enter/leave, per-entry actions, counters
  - info: run summary (directories visited, files inserted/updated)
  - warn: access errors from `FileScanner` records, skipped schedules
  - error: unexpected exceptions; safe to continue next cycle

## 11) Error Handling

- Per-entry access errors from `FileScanner` are logged and do not abort the run.
- Root-level errors for a media location (e.g., DIR_NOT_FOUND) are logged once per run at warn level (no persistence).
- Database errors: log error and continue to next file/location; run completes with partial updates.
- Error persistence: When `FileScanner` emits an error for a path under a scanned root, upsert or create a stub row keyed by `file_path` (if permissible) with `last_error_*` populated. Clear on next success for the same `file_path`.

## 12) Testing

- Unit tests (`test_files_manager.cpp`):
  - Inserts: new files in temp directories are inserted with flags reset to 0.
  - Updates: touching size/mtime resets flags to 0 and updates metadata.
  - Skip overlap: a second trigger during an active run is skipped.
  - Respects `filesservice.scan.recursive` when toggled via config.
  - Interval changes take effect on subsequent schedules.
  - Error persistence: per-entry errors populate `last_error_*`; a subsequent successful emit clears them.
  - SchedulerService: interval-driven triggering, skip when active, honors `tpm.types.fileScan.share` (0/1).
- Integration test (optional future): end-to-end with TPM and temporary config, asserting periodic runs and DB state.

## 13) Acceptance Criteria

- SchedulerService triggers `FilesManager::runOnce()` based on `files.manager.scan.intervalMs` while enabled, applying jitter/backoff/drift semantics per config.
- File scanning job (`fileScan`) is auto-registered at server startup via SchedulerService.
- Only one run may be active at a time; subsequent schedules are skipped if busy.
- New files are inserted; changed files update metadata and reset processed flags to 0.
- Honors observable scanner options and interval changes without restart.
- SchedulerService reacts to config changes (jitter/backoff/drift and job intervals) without restart.
- Shuts down cleanly with TPM draining on Ctrl+C/exit.

## 14) Scan vs Media Processing (network behavior)

- **File scan (FilesManager / FileScanner)**: Uses `std::filesystem::directory_iterator` and metadata only (`symlink_status`, `last_write_time`, `file_size`). No file content is read or transferred. For network shares this requires only directory listings and stat-like calls—lightweight, no full-file copy.
- **Media processing (MediaProcessor)**: Runs later in a separate pipeline. Uses `DiskCache::copyToCache()` to copy the full file from source (including network paths) into a local cache before ONNX/embedding work. Network traffic occurs here, not during scan.

## 15) Implementation Notes

- Relative path derivation is best-effort: when scanning directory `D`, compute `relative_path` as `path.lexically_relative(D)` if possible.
- `file_metadata` JSON is versionable; include a `schemaVersion` key if future evolution is likely.
- Network share detection can be stubbed initially on macOS/Linux and expanded later.

## 16) Follow-ups (out of immediate scope)

- Deletion tracking (identify and mark deleted files efficiently) – revisit with perf profiling and possibly batched diffs.
- Expose FilesManager status over Web API (e.g., last run time, counts).
- Backoff strategy for repeated root failures.

---

### Remaining Questions

- Scheduler scope: Should `Scheduler` be generic and reusable for other periodic tasks (e.g., future pipelines), or dedicated to `FilesManager` only?

---

File Scanner – C++-Only Implementation Spec (updated for cursor_rules.md)

Pre-req: Adhere to Project Operating Rules in cursor_rules.md. If this spec and cursor_rules.md ever conflict, cursor_rules.md wins.

⸻

0. Workflow, Commits & Scripts (from cursor_rules.md)
   • Workflow (Plan → Confirm → Implement):
   1. Draft a concise implementation plan (scope, files to touch, tests to add).
   2. Pause for user confirmation before coding.
   3. Implement in small, incremental steps, building & testing after each logical change.
      • Commit/Push: Do not commit or push unless the user explicitly asks.
      • Tests: Put all tests under /tests. Add unit tests to all_unit_tests binary (invoked by rebuild). Keep all_tests updated to run all test types.
      • Failing tests: If new failures appear, prefer fixing implementation; only modify tests as a last resort and explicitly inform the user.
      • Scripts: Put helper/one-offs under /scripts (except root build.sh, rebuild, start).

Checklist: After each task/subtask, re-read the Rules Checklist in cursor_rules.bd and fix gaps immediately.

⸻

1. Scope & Goals

Implement a C++17+ File Scanner that:
• Resides in src/filesservice/.
• Scans a specified directory, recursively by default.
• Emits a C++ struct for every file encountered and for any access error.
• Main scanning function returns no value (events carry all information).
• Exposes a comparison API to determine if a file has been modified between two emitted structs.
• Logs telemetry to console at trace level (other levels available: error, warn, info, debug).
• Subscribes to configuration changes for any used property and reacts at runtime.

Non-goals: content hashing, MIME sniffing, EXIF, persistence.

⸻

2. Project Layout

src/
filesservice/
FileScanner.h
FileScanner.cpp
FileScannerOptions.h
FileRecord.h
FileScannerErrors.h
ConfigKeys.h # config key names used by FileScanner
PlatformPaths.h
PlatformPaths.cpp
internal/
Stat.h
Stat.cpp
ShareDetection.h
ShareDetection.cpp
Logger.h # console logger (trace/debug/info/warn/error)
Logger.cpp
tests/
filesservice/
FileScannerTests.cpp
scripts/
(helper scripts if needed)

⸻

3. Public API

3.1 FileScannerOptions.h

#pragma once
namespace filesservice {

struct FileScannerOptions {
bool recursive = true; // default: true (may be overridden by config)
bool followSymlinks = false; // reserved; keep false unless needed
bool includeHidden = true; // cross-platform best-effort
};

} // namespace filesservice

3.2 FileScannerErrors.h

#pragma once
#include <string>

namespace filesservice {

enum class ErrorCode {
OK = 0,
DIR_NOT_FOUND,
PERMISSION_DENIED,
SHARE_OFFLINE,
STAT_FAILED,
SYMLINK_LOOP,
PATH_UNREADABLE,
UNKNOWN
};

const char\* to_string(ErrorCode c);

} // namespace filesservice

3.3 FileRecord.h

#pragma once
#include <cstdint>
#include <string>
#include <chrono>

#include "FileScannerErrors.h"

namespace filesservice {

using TimePoint = std::chrono::system_clock::time_point;

struct FileRecord {
// Identity & pathing
std::string fileName; // basename
std::string fullPath; // absolute, native separators
std::string extension; // lowercased ("" if none)

// Network share awareness
std::string networkSharePath; // UNC or mount root; empty if N/A
std::string networkShareDriveName; // e.g., "Z:" or volume name; empty if N/A
bool isShareMapped = false; // true if accessible at emit time

// Stats
TimePoint createdAt{}; // UTC
TimePoint modifiedAt{}; // UTC
std::uint64_t fileSizeBytes = 0;

// Low-level IDs
std::string deviceId; // volume serial or st_dev
std::string inode; // file index or st_ino

// Attributes
std::string symlinkTarget; // absolute target if symlink; else empty
bool isHidden = false;

// Error (for successes: error == OK)
ErrorCode error = ErrorCode::OK;
int platformErrno = 0;
std::string errorMessage;

bool hasError() const noexcept { return error != ErrorCode::OK; }
};

enum class ChangeKind {
Unchanged,
SizeOrTimeChanged,
IdentityChanged,
AttributesChanged
};

} // namespace filesservice

3.4 FileScanner.h

#pragma once
#include <functional>
#include <filesystem>
#include "FileRecord.h"
#include "FileScannerOptions.h"

namespace filesservice {

using FileCallback = std::function<void(const FileRecord&)>;

/\*\*

- Scan a directory and emit a FileRecord for each file or access error.
- Returns no value; results stream via callback.
-
- Root-unreadable behavior:
- - Emit one FileRecord with .fullPath=root and error set appropriately,
-     then return.
  \*/
  void scan(const std::filesystem::path& directory,
  const FileScannerOptions& options,
  const FileCallback& onFile);

/\*_ Overload with default options (recursive=true). _/
inline void scan(const std::filesystem::path& directory,
const FileCallback& onFile) {
scan(directory, FileScannerOptions{}, onFile);
}

/\*_ True if a "meaningful" change occurred (identity/size/time/attributes). _/
bool isModified(const FileRecord& previous, const FileRecord& current);

/\*_ Classification for finer control over change semantics. _/
ChangeKind classifyChange(const FileRecord& previous,
const FileRecord& current);

} // namespace filesservice

⸻

4. Behavior & Semantics 1. Traversal
   • recursive defaults to true (unless configuration overrides at runtime).
   • Non-recursive scans only enumerate top-level entries.
   • Per-entry success → emit FileRecord with error=OK.
   Per-entry failure → emit FileRecord with error!=OK, platformErrno, errorMessage. Continue. 2. Root errors
   • Missing/not a directory → emit one record with DIR_NOT_FOUND and return.
   • Root permission denied → emit one record with PERMISSION_DENIED and return. 3. Symlinks & loops
   • With followSymlinks=false, emit the symlink itself; set symlinkTarget if resolvable.
   • Maintain a visited set of (deviceId, inode) to avoid cycles. 4. Network shares
   • Windows: detect \\server\share and mapped drives (e.g., Z:). Set networkSharePath, networkShareDriveName, isShareMapped. If offline, set isShareMapped=false and use SHARE_OFFLINE where applicable.
   • macOS: detect /Volumes/<Name>.
   • Linux: detect CIFS/SMB/NFS via /proc/mounts or statfs. 5. Timestamps
   • Convert to UTC TimePoint. Prefer birth/creation time if available; otherwise documented fallback. 6. Hidden files
   • Windows hidden attribute; Unix prefix .. 7. No accumulation
   • Stream via onFile immediately; do not store the entire result set.

⸻

5. Change Detection
   • Identity change → (deviceId, inode) differs ⇒ IdentityChanged (treat as modified).
   • Fast metadata change → fileSizeBytes or modifiedAt differs ⇒ SizeOrTimeChanged.
   • Attribute-only change → share fields / isHidden / symlinkTarget differ, identity & fast metadata same ⇒ AttributesChanged.
   • Unchanged → none of the above.

inline bool isModified(const FileRecord& a, const FileRecord& b) {
return classifyChange(a, b) != ChangeKind::Unchanged;
}

⸻

6. Configuration Integration (required by cursor_rules.bd)
   • Define config keys in ConfigKeys.h (example names):
   • filesservice.scan.recursive (bool; default: true)
   • filesservice.scan.followSymlinks (bool; default: false)
   • filesservice.scan.includeHidden (bool; default: true)
   • filesservice.log.level (string: "trace"|"debug"|"info"|"warn"|"error"; default: "trace")
   • Subscribe to config changes at runtime:
   • When a key changes, the scanner must immediately update in-memory behavior for new scans.
   • Keep the default config generator in sync with new/changed keys.
   • Update config reference/docs whenever keys are added/changed.

Implementation detail: provide a small adapter (e.g., ConfigAdapter) that reads current values on scan() start, and registers change callbacks with the host config system (whatever the project uses).

⸻

7. Logging (console)
   • Implement a minimal console logger (internal/Logger.\*) with levels: trace, debug, info, warn, error.
   • Default level = trace (telemetry-heavy), can be overridden by filesservice.log.level.
   • Suggested events:
   • trace: scan start/stop, directory enter/leave, per-entry decisions, symlink handling, share mapping results, emission counts.
   • debug: mount table parsing, platform timing conversions.
   • info: root summary (dirs visited, files emitted).
   • warn: recoverable access errors (also emitted as error records).
   • error: root-level failure records (also emitted).

⸻

8. Implementation Notes
   • Use std::filesystem for traversal and absolute normalization; keep native separators in fullPath.
   • Platform IDs:
   • Windows: GetFileInformationByHandleEx → volume serial + file index.
   • POSIX: stat.st_dev / stat.st_ino.
   • Lowercase extension (strip leading dot).
   • Always emit something for every encountered entry (success or error), ensuring a complete audit trail.

⸻

9. Testing (per cursor_rules.bd unified policy)
   • Location: /tests/filesservice/FileScannerTests.cpp.
   • Add tests to all_unit_tests; keep all_tests running all types.
   • Minimum unit tests:
   1. Root missing → single DIR_NOT_FOUND record then return.
   2. Non-recursive vs recursive enumeration counts.
   3. Emits records for files w/ and w/o extensions.
   4. Hidden detection (Windows attribute & dotfiles).
   5. Symlink loop avoided; symlinkTarget set when resolvable.
   6. Permission-denied subtree emits error records and continues.
   7. Network share detection (platform-stubbed): mapped vs offline.
   8. Timestamp normalization sanity.
   9. Large tree streaming (callback increments; no bulk memory).
   10. classifyChange cases: Unchanged / SizeOrTimeChanged / IdentityChanged / AttributesChanged.
   11. Config change reactivity: changing filesservice.scan.recursive and filesservice.log.level between scans takes effect without restart.

After each logical test or feature, build & run tests before proceeding.

⸻

10. Acceptance Criteria
    • Implementation under src/filesservice/; helper scripts under /scripts (if any).
    • scan(path, options, onFile) compiles and returns void; default overload uses recursion.
    • For every file or access error, a FileRecord is emitted; errors live inside the struct.
    • isModified and classifyChange behave per §5.
    • Console logging at trace by default; other levels available.
    • Configuration: reads defaults, subscribes to runtime changes, updates behavior on next scan; default config generator & docs updated.
    • Tests located under /tests, integrated into all_unit_tests and all_tests.
    • Team followed Workflow & Rules Checklist from cursor_rules.bd.

⸻

11. Example

#include "filesservice/FileScanner.h"
#include <iostream>

using namespace filesservice;

int main() {
// Use defaults; respects current config values at call time.
scan("Z:/Projects", [](const FileRecord& rec) {
if (rec.hasError()) {
std::cerr << "[error] " << rec.fullPath << " -> "
<< to_string(rec.error) << " : " << rec.errorMessage
<< " (errno=" << rec.platformErrno << ")\n";
return;
}
std::cout << "[file] " << rec.fullPath << " (" << rec.fileSizeBytes << " bytes)\n";
});
}

⸻

End of spec (aligned with cursor_rules.bd).

