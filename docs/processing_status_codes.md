# Media Processing Status Codes

This document defines the status codes used in the `scanned_files` table to track file processing states.

## Status Code Reference

| Code  | Description                     | Usage                                                                                                       |
| ----- | ------------------------------- | ----------------------------------------------------------------------------------------------------------- |
| `0`   | **Unprocessed**                 | File has been scanned but not yet processed                                                                 |
| `1`   | **Currently Processing**        | File is currently being processed by a worker thread                                                        |
| `2`   | **Processing Complete**         | File has been successfully processed                                                                        |
| `>2`  | **Processing Complete**         | File has been successfully processed (value indicates processing timestamp or version)                      |
| `-99` | **Queued for Processing**       | File has been submitted to the processing queue but not yet picked up by a worker thread                    |
| `-2`  | **Skipped Due to Backpressure** | File was skipped because processing queue was at capacity (retryable)                                       |
| `-1`  | **General Error**               | File processing failed due to corruption, format issues, or other general errors                            |
| `-3`  | **File Access Error**           | File processing failed due to file access issues (permission denied, file not found, file locked)           |
| `-4`  | **RAW Validation Error**        | RAW file failed LibRaw validation (unsupported format, corrupted header, truncated file)                    |
| `-5`  | **TIFF Validation Error**       | TIFF file failed libtiff validation (corrupted header, invalid IFD, missing required tags)                  |
| `-6`  | **Cache Operation Error**       | File processing failed due to disk cache issues (cache copy failed, cache save failed, cache delete failed) |

## Implementation Notes

- **Positive values** (>0): Indicate successful processing completion
- **Zero** (0): Indicates unprocessed state
- **-99**: Indicates queued for processing state
- **-98 to -1**: Indicate various failure conditions

## Error Code Rationale

The negative error codes support retry logic with escalation:

- **-99**: Queued for processing - retryable (will be picked up by worker thread)
- **-2**: Backpressure errors (queue at capacity) - retryable immediately, not escalated
- **-1**: General errors (corruption, format issues) - retryable once
- **-3**: File access errors may be temporary (file locked by another process) - retryable once
- **-4**: Memory errors may be temporary (system under load) - retryable once
- **-5**: Network errors are often temporary (network connectivity issues) - retryable once
- **-6**: Cache errors may be temporary (disk space issues, file system errors) - retryable once

## Retry Logic and Error Escalation

The system implements a **single retry with escalation** policy:

1. **Initial Error**: Files that fail processing are marked with standard error codes (-99, -2, -1, -3, -4, -5, -6)
2. **Retry Attempt**: Files with error codes >= -100 are eligible for retry
3. **Escalation**: If a file fails again after retry, the error code is escalated by subtracting 100:
   - -1 (General Error) → -101 (Escalated General Error)
   - -3 (File Access Error) → -103 (Escalated File Access Error)
   - -4 (Memory Error) → -104 (Escalated Memory Error)
   - -5 (Network Error) → -105 (Escalated Network Error)
   - -6 (Cache Error) → -106 (Escalated Cache Error)
   - -99 (Queued) → -199 (Escalated Queued - should not occur)
4. **No Further Retries**: Files with error codes < -100 are never retried again
5. **Special Case -2**: Backpressure files (-2) are retried immediately when queue capacity becomes available and are never escalated

### Escalated Error Codes

| Code   | Description                      | Retry Status                          |
| ------ | -------------------------------- | ------------------------------------- |
| `-101` | **Escalated General Error**      | No further retries                    |
| `-102` | **Escalated Backpressure Error** | No further retries (should not occur) |
| `-103` | **Escalated File Access Error**  | No further retries                    |
| `-104` | **Escalated Memory Error**       | No further retries                    |
| `-105` | **Escalated Network Error**      | No further retries                    |
| `-106` | **Escalated Cache Error**        | No further retries                    |
| `-199` | **Escalated Queued Error**       | No further retries (should not occur) |

## Error Count Reporting

The server status endpoint (`/api/v1/server/status`) reports `error_files_count` which reflects **true processing errors only**:

**Counted as Errors:**

- ✅ `-1` (General Error)
- ✅ `-3` (File Access Error)
- ✅ `-4` (Memory Error)
- ✅ `-5` (Network Error)
- ✅ `-6` (Cache Error)
- ✅ `-101` to `-106` (Escalated Errors)

**NOT Counted as Errors:**

- ❌ `-2` (Backpressure) - Transient queue capacity issue, automatically retried
- ❌ `-99` (Queued) - Files waiting to be processed, reported separately as `queued_files_count`

This distinction ensures that the error count reflects actual processing failures rather than temporary operational states. Backpressure files are automatically retried when queue capacity becomes available, and queued files are actively waiting to be processed.

## Reset Errors Operation

The "Reset All Errors" operation (`POST /api/v1/files/reset-errors`) resets error files to unprocessed status, allowing them to be retried. The operation uses the same filtering logic as error count reporting.

**Files that ARE reset (actual errors):**

- ✅ `-1` (General Error) → `0` (Unprocessed)
- ✅ `-3` (File Access Error) → `0` (Unprocessed)
- ✅ `-4` (Memory Error) → `0` (Unprocessed)
- ✅ `-5` (Network Error) → `0` (Unprocessed)
- ✅ `-6` (Cache Error) → `0` (Unprocessed)
- ✅ All escalated errors (`< -100`) → `0` (Unprocessed)

**Files that are NOT reset (temporary states):**

- ❌ `-2` (Backpressure): Temporary queue full state, will retry automatically
- ❌ `-99` (Queued): Already in processing queue
- ❌ `0` (Unprocessed): Already in correct state
- ❌ `1` (Processing): Currently being processed
- ❌ `2` (Complete): Successfully processed

**SQL Implementation:**

```sql
-- FAST mode reset
UPDATE scanned_files SET processed_fast=0
WHERE processed_fast < 0 AND processed_fast != -2 AND processed_fast != -99;

-- BALANCED mode reset
UPDATE scanned_files SET processed_balanced=0
WHERE processed_balanced < 0 AND processed_balanced != -2 AND processed_balanced != -99;

-- QUALITY mode reset
UPDATE scanned_files SET processed_quality=0
WHERE processed_quality < 0 AND processed_quality != -2 AND processed_quality != -99;
```

This ensures consistency with error count reporting and preserves operational states that are not actual errors.

## Database Schema

The `scanned_files` table uses the `status` column to store these codes:

```sql
CREATE TABLE scanned_files (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    file_path TEXT UNIQUE NOT NULL,
    status INTEGER NOT NULL DEFAULT 0,
    -- ... other columns
);
```

## Query Examples

```sql
-- List all unprocessed files (including retryable errors and backpressure)
SELECT file_path FROM scanned_files WHERE processed_fast = 0 OR processed_fast = -99 OR (processed_fast >= -100 AND processed_fast < 0);

-- List files that failed due to file access issues
SELECT file_path FROM scanned_files WHERE processed_fast = -3;

-- List files that need retry (all retryable errors >= -100, including backpressure)
SELECT file_path FROM scanned_files WHERE processed_fast = -99 OR (processed_fast >= -100 AND processed_fast < 0);

-- List files currently queued for processing
SELECT file_path FROM scanned_files WHERE processed_fast = -99;

-- List files that will never be retried (escalated errors < -100)
SELECT file_path FROM scanned_files WHERE processed_fast < -100;

-- List backpressure files (retryable immediately)
SELECT file_path FROM scanned_files WHERE processed_fast = -2;

-- List files with specific escalated errors
SELECT file_path FROM scanned_files WHERE processed_fast = -101; -- Escalated general error
SELECT file_path FROM scanned_files WHERE processed_fast = -103; -- Escalated file access error
```

---

## Error Message Persistence

### Processing Errors Table

The system maintains a separate `processing_errors` table to record detailed error information when files fail processing. This provides historical context and debugging information beyond the status codes stored in `scanned_files`.

#### Table Schema

```sql
CREATE TABLE processing_errors (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    file_path TEXT NOT NULL,
    server_mode TEXT NOT NULL,  -- 'FAST', 'BALANCED', 'QUALITY'
    error_code INTEGER NOT NULL,
    error_message TEXT NOT NULL,
    error_source TEXT,  -- 'ImageMagick', 'OpenCV', 'ONNX', 'Memory', 'FileSystem', 'Network', 'General'
    timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Indexes for efficient queries
CREATE INDEX idx_processing_errors_file_path ON processing_errors(file_path);
CREATE INDEX idx_processing_errors_timestamp ON processing_errors(timestamp DESC);
```

#### Error Sources

| Source        | Description                     |
| ------------- | ------------------------------- |
| `ImageMagick` | RAW file transcoding errors     |
| `OpenCV`      | Image reading/processing errors |
| `ONNX`        | Neural network inference errors |
| `Memory`      | Memory allocation failures      |
| `FileSystem`  | File access/permission errors   |
| `Network`     | Network-related errors          |
| `General`     | Other unclassified errors       |

#### Usage

Errors are automatically logged when files fail processing. Multiple error records can exist for the same file across different processing attempts and modes.

#### Query Examples

```sql
-- Get all error details for a specific file
SELECT * FROM processing_errors
WHERE file_path = '/path/to/file.jpg'
ORDER BY timestamp DESC;

-- Find files with OpenCV errors in QUALITY mode
SELECT DISTINCT file_path, error_message
FROM processing_errors
WHERE error_source = 'OpenCV' AND server_mode = 'QUALITY'
ORDER BY timestamp DESC LIMIT 100;

-- Count errors by source in the last 24 hours
SELECT error_source, COUNT(*) as error_count
FROM processing_errors
WHERE timestamp > datetime('now', '-24 hours')
GROUP BY error_source
ORDER BY error_count DESC;

-- Get detailed error information for escalated errors (-101)
SELECT sf.file_path, sf.processed_quality, pe.error_message, pe.error_source, pe.timestamp
FROM scanned_files sf
JOIN processing_errors pe ON sf.file_path = pe.file_path
WHERE sf.processed_quality = -101 AND pe.server_mode = 'QUALITY'
ORDER BY pe.timestamp DESC;

-- Find most common error messages
SELECT error_message, COUNT(*) as occurrences
FROM processing_errors
WHERE timestamp > datetime('now', '-7 days')
GROUP BY error_message
ORDER BY occurrences DESC
LIMIT 20;
```

#### Design Rationale

- **Separation of Concerns**: Error details are kept separate from operational state (`scanned_files`)
- **Historical Record**: All error attempts are preserved, not just the latest
- **Write-Optimized**: Insert-only operations, no foreign key constraints for maximum performance
- **Debugging**: Detailed error messages from exceptions captured for post-event analysis
- **No Impact on Processing**: Error logging failures don't block file processing
