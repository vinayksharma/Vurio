# Server Startup Scan Functionality

## Overview

The dedup server now performs an **immediate scan** of all configured directories as soon as it starts up, followed by the normal scheduled processing. This ensures that files are discovered and processed immediately upon server startup, rather than waiting for the next scheduled scan interval.

## Implementation Details

### 1. Startup Flow

**New startup sequence:**

```
1. Server initialization (config, database, managers)
2. DuplicateLinker startup
3. IMMEDIATE SCAN of all configured directories ← NEW
4. IMMEDIATE PROCESSING of discovered files ← NEW
5. Scheduler startup with normal scheduled operations
6. HTTP server startup
```

### 2. Immediate Scan Implementation

**Location:** `src/main.cpp` - Added after DuplicateLinker startup, before scheduler setup

**Features:**

- **Parallel scanning** using TBB with configured thread count
- **Round-robin distribution** of scan paths across threads
- **Thread-safe counters** for progress tracking
- **Database locking** to prevent race conditions
- **Comprehensive error handling** and logging

**Code Structure:**

```cpp
// Perform immediate scan on startup
Logger::info("Performing immediate scan on server startup...");
try {
    // Get all stored scan paths from database
    auto scan_paths = db_manager.getUserInputs("scan_path");

    // Parallel scanning with TBB
    tbb::parallel_for(tbb::blocked_range<size_t>(0, scan_paths.size()),
        [&](const tbb::blocked_range<size_t>& range) {
            // Round-robin distribution logic
            // File scanning with database locking
        });

    // Log completion statistics
    Logger::info("Immediate startup scan completed - Total files stored: " +
                 std::to_string(total_files_stored.load()));
} catch (const std::exception &e) {
    Logger::error("Error in immediate startup scan: " + std::string(e.what()));
}
```

### 3. Immediate Processing

**Triggered when:** Files are found during startup scan

**Implementation:**

```cpp
// If files were found, trigger immediate processing
if (total_files_stored.load() > 0) {
    Logger::info("Files found during startup scan, triggering immediate processing...");
    try {
        ThreadPoolManager::processAllScannedFilesAsync(
            config_manager.getMaxProcessingThreads(),
            // Event callbacks for progress tracking
        );
    } catch (const std::exception &e) {
        Logger::error("Error in startup processing: " + std::string(e.what()));
    }
} else {
    Logger::info("No files found during startup scan, skipping immediate processing");
}
```

### 4. Threading and Performance

**Scan Threading:**

- Uses `config_manager.getMaxScanThreads()` for parallel scanning
- TBB parallel_for with round-robin distribution
- Each thread processes every Nth path for load balancing

**Processing Threading:**

- Uses `config_manager.getMaxProcessingThreads()` for file processing
- ThreadPoolManager handles concurrent file processing
- Non-blocking async processing with callbacks

**Database Safety:**

- TBB mutex prevents race conditions during scanning
- Each thread creates its own FileScanner instance
- Atomic counters for thread-safe progress tracking

## Configuration

**No new configuration required:**

- Uses existing `max_scan_threads` setting
- Uses existing `max_processing_threads` setting
- Uses existing scan paths from database

**Existing settings that affect startup scan:**

```yaml
# config.yaml
max_scan_threads: 4 # Number of parallel scan threads
max_processing_threads: 4 # Number of parallel processing threads
```

## Logging and Monitoring

### Startup Scan Logs

**Begin:**

```
[INFO] Performing immediate scan on server startup...
[INFO] Found X scan paths for immediate scan
[INFO] Starting immediate parallel scan with X threads for X scan paths
```

**Progress:**

```
[INFO] Thread X scanning directory: /path/to/directory
[INFO] Thread X completed scan for /path/to/directory - Files stored: X
```

**Completion:**

```
[INFO] Immediate startup scan completed - Total files stored: X, Successful scans: X, Failed scans: X
[INFO] All immediate startup scans completed - Total files stored: X
```

### Immediate Processing Logs

**Trigger:**

```
[INFO] Files found during startup scan, triggering immediate processing...
```

**Progress:**

```
[INFO] Startup processing: /path/to/file (format: jpeg, confidence: 95)
[WARN] Startup processing failed: /path/to/file - Error message
```

**Completion:**

```
[INFO] Startup processing completed
```

## Benefits

### 1. Immediate File Discovery

- **No waiting** for scheduled scan intervals
- **Instant processing** of existing files
- **Faster response** to server restarts

### 2. Improved User Experience

- **Files processed immediately** after server startup
- **No delay** in duplicate detection
- **Consistent behavior** regardless of restart timing

### 3. Better Resource Utilization

- **Parallel scanning** maximizes CPU usage
- **Immediate processing** reduces idle time
- **Efficient startup** sequence

### 4. Production Readiness

- **Server ready** for requests immediately after startup
- **All files processed** before accepting new work
- **Consistent state** from startup

## Testing

**Test Script:** `tests/scripts/test_startup_scan.sh`

**Test Coverage:**

1. **Startup scan execution** - Verifies immediate scan runs
2. **File discovery** - Checks if files are found and stored
3. **Immediate processing** - Validates file processing triggers
4. **Restart behavior** - Tests scan behavior on server restart
5. **Database consistency** - Verifies data integrity

**Test Execution:**

```bash
./tests/scripts/test_startup_scan.sh
```

## Performance Considerations

### 1. Startup Time

- **Additional startup time** proportional to scan directory size
- **Parallel execution** minimizes impact
- **Non-blocking** - server starts accepting requests after scan

### 2. Memory Usage

- **Temporary memory** for parallel scanning
- **Thread pool** for concurrent processing
- **Database connections** managed safely

### 3. Resource Contention

- **Database locking** prevents race conditions
- **Thread limits** prevent resource exhaustion
- **Error handling** ensures graceful degradation

## Error Handling

### 1. Scan Failures

- **Individual path failures** don't stop other scans
- **Comprehensive logging** of all errors
- **Graceful degradation** if scan fails

### 2. Processing Failures

- **File processing errors** logged but don't stop startup
- **Thread pool errors** handled gracefully
- **Database errors** logged with context

### 3. Startup Failures

- **Critical errors** prevent server startup
- **Non-critical errors** logged but don't stop server
- **Fallback behavior** for missing scan paths

## Future Enhancements

### 1. Configurable Startup Behavior

- **Option to disable** immediate scan
- **Configurable scan delay** after startup
- **Selective startup scanning** for specific paths

### 2. Progress Reporting

- **Real-time progress** via API endpoints
- **Startup status** in server status API
- **WebSocket notifications** for scan progress

### 3. Smart Startup

- **Incremental scanning** based on last scan time
- **Priority-based scanning** for important directories
- **Background scanning** after server is ready

## Troubleshooting

### Common Issues

**1. Startup Scan Taking Too Long:**

- Check `max_scan_threads` configuration
- Verify scan directory sizes and file counts
- Monitor system resources during startup

**2. Files Not Being Processed:**

- Check `max_processing_threads` configuration
- Verify ThreadPoolManager initialization
- Check database connection status

**3. Memory Issues During Startup:**

- Reduce `max_scan_threads` and `max_processing_threads`
- Monitor system memory usage
- Check for memory leaks in scanning code

### Debug Information

**Enable debug logging:**

```yaml
# config.yaml
log_level: DEBUG
```

**Check startup logs:**

```bash
# Look for startup scan messages
grep "immediate\|startup" server.log
```

**Monitor resource usage:**

```bash
# Check memory and CPU during startup
top -p $(cat dedup_server.pid)
```

## Conclusion

The startup scan functionality provides immediate file discovery and processing upon server startup, significantly improving the user experience and server readiness. The implementation is robust, performant, and maintains all existing scheduled functionality while adding this valuable startup behavior.
