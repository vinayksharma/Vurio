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
| `-4`  | **Memory Allocation Error**     | File processing failed due to memory allocation issues (out of memory, allocation failure)                  |
| `-5`  | **Network-Related Error**       | File processing failed due to network issues (network files, connection timeouts)                           |
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
