# Smart Cache Cleanup System

## Overview

The Smart Cache Cleanup system provides intelligent, processing-status-aware cache management for the dedup server. Unlike basic cache cleanup that only considers file age and size, this system analyzes the processing status of transcoded files to make informed decisions about which files to remove.

## Key Features

### 1. **Processing Status Awareness**

- **Fully Processed**: Files processed in all enabled quality modes
- **Partially Processed**: Files processed in some quality modes
- **Unprocessed**: Files not yet processed in any quality mode
- **Source Missing**: Files where the original source no longer exists

### 2. **Intelligent Prioritization**

The cleanup follows a smart priority order:

1. **Priority 1**: Remove invalid files (source changed/missing)
2. **Priority 2**: Remove processed + old transcoded files
3. **Priority 3**: Remove unprocessed + old transcoded files
4. **Priority 4**: Remove oldest valid files (if still over limit)

### 3. **Configurable Age Thresholds**

- **Fully Processed**: Configurable retention period (default: 7 days)
- **Partially Processed**: Configurable retention period (default: 3 days)
- **Unprocessed**: Configurable retention period (default: 1 day)

### 4. **Runtime Configuration**

All cleanup parameters can be adjusted at runtime without server restart.

## API Endpoints

### Cache Status

```http
GET /api/cache/status
```

Returns current cache size, limits, and cleanup configuration.

### Cache Cleanup

```http
POST /api/cache/cleanup
Content-Type: application/json

{
    "type": "smart"  // "smart", "enhanced", or "basic"
}
```

Initiates cache cleanup with specified strategy.

### Cache Configuration

```http
GET /api/cache/config
```

Returns current cleanup configuration.

```http
POST /api/cache/config
Content-Type: application/json

{
    "fully_processed_age_days": 14,
    "partially_processed_age_days": 7,
    "unprocessed_age_days": 2,
    "require_all_modes": false,
    "cleanup_threshold_percent": 75
}
```

Updates cleanup configuration.

## Configuration

### Default Settings

```yaml
cache_cleanup:
  fully_processed_age_days: 7 # Remove fully processed files older than 7 days
  partially_processed_age_days: 3 # Remove partially processed files older than 3 days
  unprocessed_age_days: 1 # Remove unprocessed files older than 1 day
  require_all_modes: true # Require all modes to be processed for "fully processed"
  cleanup_threshold_percent: 80 # Start cleanup when 80% full
```

### Runtime Configuration

```cpp
auto &transcoding_manager = TranscodingManager::getInstance();

// Set custom cleanup configuration
transcoding_manager.setCleanupConfig(
    14,    // Fully processed: 14 days
    7,     // Partially processed: 7 days
    2,     // Unprocessed: 2 days
    false, // Don't require all modes
    75     // Cleanup threshold: 75%
);

// Get current configuration
auto config = transcoding_manager.getCleanupConfig();
```

## Usage Examples

### Basic Usage

```cpp
// Automatic cleanup during transcoding
if (transcoding_manager.isCacheOverLimit()) {
    transcoding_manager.cleanupCacheSmart(true);
}

// Manual cleanup
size_t files_removed = transcoding_manager.cleanupCacheSmart(false);
```

### Advanced Usage

```cpp
// Get cache entries with processing status
auto entries = transcoding_manager.getCacheEntriesWithStatus();

// Analyze cache entries
for (const auto& entry : entries) {
    std::cout << "File: " << entry.source_file << std::endl;
    std::cout << "Status: " << entry.processing_status << std::endl;
    std::cout << "Age: " << entry.cache_age << " seconds" << std::endl;
    std::cout << "Size: " << entry.file_size << " bytes" << std::endl;
}
```

## Benefits

### 1. **Efficient Space Management**

- Prioritizes removal of files that have already served their purpose
- Preserves cache for files still being processed
- Maximizes cache hit rate for active processing

### 2. **Processing Efficiency**

- Keeps recently transcoded files for active processing
- Removes old processed files that won't be needed again
- Reduces unnecessary re-transcoding

### 3. **Better Resource Utilization**

- Cache space used more efficiently
- Prioritizes files based on actual usage patterns
- Reduces disk I/O for re-transcoding

### 4. **User Experience**

- Faster processing of new files
- Better cache hit rates for active workflows
- More predictable cache behavior

## Implementation Details

### Cache Entry Structure

```cpp
struct CacheEntry {
    std::string source_file;           // Original file path
    std::string cache_file;            // Transcoding cache file path
    bool is_processed;                 // Processed in at least one mode
    bool is_fully_processed;           // Processed in all enabled modes
    std::time_t cache_age;             // How old is the transcoded file
    size_t file_size;                  // Cache file size
    std::string processing_status;     // Human-readable processing status
};
```

### Cleanup Phases

```cpp
size_t TranscodingManager::cleanupCacheSmart(bool force_cleanup)
{
    // Phase 1: Remove invalid files (source changed/missing)
    size_t invalid_removed = removeInvalidFiles(cache_entries);

    // Phase 2: Remove processed old files
    size_t processed_removed = removeProcessedOldFiles(cache_entries);

    // Phase 3: Remove unprocessed old files
    size_t unprocessed_removed = removeUnprocessedOldFiles(cache_entries);

    // Phase 4: If still over limit, remove oldest valid files
    size_t oldest_removed = 0;
    if (getCacheSize() > max_size) {
        oldest_removed = removeOldestValidFiles(cache_entries);
    }

    return invalid_removed + processed_removed + unprocessed_removed + oldest_removed;
}
```

## Testing

### Test Program

```bash
# Build the test program
g++ -o smart_cache_test examples/smart_cache_cleanup_test.cpp -I./include -L./build -ldedup_server

# Run the test
./smart_cache_test
```

### Test Scenarios

1. **Basic Cleanup**: Test traditional age-based cleanup
2. **Enhanced Cleanup**: Test source file validation
3. **Smart Cleanup**: Test processing status-aware cleanup
4. **Force Cleanup**: Test cleanup regardless of size limits
5. **Configuration**: Test runtime configuration changes

## Performance Considerations

### Database Queries

- Efficient queries for processing status
- Indexed lookups for cache entries
- Batch processing for large cache directories

### Memory Management

- Configurable batch sizes for cleanup operations
- Thread-safe operations with proper mutex protection
- Efficient file system operations

### Monitoring

- Detailed logging for all cleanup operations
- Performance metrics for cleanup efficiency
- Configurable cleanup thresholds

## Future Enhancements

### 1. **Machine Learning Integration**

- Predict file usage patterns
- Adaptive cleanup thresholds
- Intelligent retention policies

### 2. **Advanced Analytics**

- Cache hit rate optimization
- Processing workflow analysis
- Storage efficiency metrics

### 3. **Distributed Cache**

- Multi-server cache coordination
- Load balancing for cache operations
- Cross-server cache invalidation

## Troubleshooting

### Common Issues

1. **Cache Not Cleaning Up**

   - Check cleanup configuration
   - Verify file permissions
   - Review database connectivity

2. **Performance Issues**

   - Adjust batch sizes
   - Review cleanup thresholds
   - Monitor database performance

3. **Configuration Errors**
   - Validate configuration values
   - Check configuration file syntax
   - Review error logs

### Debug Information

```cpp
// Enable debug logging
Logger::setLevel(LogLevel::DEBUG);

// Check cache status
auto status = transcoding_manager.getCacheSizeString();
auto config = transcoding_manager.getCleanupConfig();

// Monitor cleanup operations
size_t removed = transcoding_manager.cleanupCacheSmart(true);
Logger::info("Cleanup completed: " + std::to_string(removed) + " files removed");
```

## Conclusion

The Smart Cache Cleanup system provides a significant improvement over basic cache management by considering the actual processing status of files. This results in more efficient cache utilization, better performance, and improved user experience while maintaining system reliability and configurability.
