# Media Processing Status Codes

This document defines the status codes used in the `scanned_files` table to track file processing states.

## Status Code Reference

| Code | Description                     | Usage                                                                                                       |
| ---- | ------------------------------- | ----------------------------------------------------------------------------------------------------------- |
| `0`  | **Unprocessed**                 | File has been scanned but not yet processed                                                                 |
| `>0` | **Processing Complete**         | File has been successfully processed (value indicates processing timestamp or version)                      |
| `-1` | **General Error**               | File processing failed due to corruption, format issues, or other general errors                            |
| `-2` | **Skipped Due to Backpressure** | File was skipped because processing queue was at capacity                                                   |
| `-3` | **File Access Error**           | File processing failed due to file access issues (permission denied, file not found, file locked)           |
| `-4` | **Memory Allocation Error**     | File processing failed due to memory allocation issues (out of memory, allocation failure)                  |
| `-5` | **Network-Related Error**       | File processing failed due to network issues (network files, connection timeouts)                           |
| `-6` | **Cache Operation Error**       | File processing failed due to disk cache issues (cache copy failed, cache save failed, cache delete failed) |

## Implementation Notes

- **Positive values** (>0): Indicate successful processing completion
- **Zero** (0): Indicates unprocessed state
- **Negative values** (<0): Indicate various failure conditions

## Error Code Rationale

The negative error codes are designed to support future retry logic implementation:

- **-3**: File access errors may be temporary (file locked by another process)
- **-4**: Memory errors may be temporary (system under load)
- **-5**: Network errors are often temporary (network connectivity issues)
- **-6**: Cache errors may be temporary (disk space issues, file system errors)

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
-- List all unprocessed files
SELECT file_path FROM scanned_files WHERE status = 0;

-- List files that failed due to file access issues
SELECT file_path FROM scanned_files WHERE status = -3;

-- List files that need retry (network, memory, or cache errors)
SELECT file_path FROM scanned_files WHERE status IN (-4, -5, -6);
```
