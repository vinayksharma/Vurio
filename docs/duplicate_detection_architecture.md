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
  └─ Update checkpoint
```

**Key Principles:**

- **Representative-Based:** New files compared ONLY against group representatives, not all members
- **Non-Transitive:** If A~B and B~C but A≁C, they will be in different groups
- **Highest Similarity:** If multiple representatives match, add to group with highest similarity
- **2+ Required:** New groups only created when 2+ similar ungrouped files found in batch

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

### Similarity Thresholds

| Key                             | Default | Description                                         |
| ------------------------------- | ------- | --------------------------------------------------- |
| `duplicates.fast.threshold`     | `0.90`  | pHash similarity (0.85-0.95 recommended)            |
| `duplicates.balanced.threshold` | `0.30`  | Feature match ratio (0.20-0.40 recommended)         |
| `duplicates.quality.threshold`  | `0.95`  | Embedding cosine similarity (0.90-0.98 recommended) |

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
