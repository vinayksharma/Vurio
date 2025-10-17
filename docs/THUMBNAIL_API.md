# Thumbnail API Documentation

## Overview

The Thumbnail API provides on-demand generation and caching of image thumbnails with intelligent cache management. Thumbnails are generated once and cached on disk, with database metadata tracking for fast lookup.

## Features

✅ **On-Demand Generation**: Generates thumbnails only when requested  
✅ **Intelligent Caching**: Database-backed cache with file system storage  
✅ **Multiple Sizes**: Supports 128, 256, 512, and 1024 pixel thumbnails  
✅ **Aspect Ratio Preservation**: Maintains original image proportions  
✅ **Automatic Invalidation**: Detects source file changes and regenerates  
✅ **Thread-Safe**: Uses TPM for concurrent thumbnail generation  
✅ **HTTP Caching**: Proper cache headers for browser/CDN caching  
✅ **RAW File Support**: Automatically transcodes RAW files (ARW, CR2, NEF, DNG) to TIFF before thumbnail generation  
✅ **Configuration-Aware**: Respects transcoding configuration (enabled/disabled, timeout, etc.)

## API Endpoints

### GET /api/v1/thumbnails

Generate or retrieve a cached thumbnail for an image.

**Query Parameters:**

- `path` (required): Source image file path (URL-encoded)
- `size` (optional): Thumbnail size - must be one of: `128`, `256`, `512`, `1024`. Default: `256`

**Response:**

- **200 OK**: JPEG image with caching headers
- **400 Bad Request**: Invalid parameters (missing path or invalid size)
- **404 Not Found**: Source file doesn't exist or is not an image
- **500 Internal Server Error**: Generation failed
- **504 Gateway Timeout**: Generation exceeded timeout limit

**Response Headers:**

```
Content-Type: image/jpeg
Cache-Control: public, max-age=31536000, immutable
Last-Modified: <source_file_mtime>
ETag: "<hash>-<mtime>-<size>"
Content-Length: <bytes>
```

**Example:**

```bash
# Generate 256px thumbnail
curl "http://localhost:8080/api/v1/thumbnails?path=/photos/image.jpg&size=256" \
  --output thumbnail.jpg

# Generate 512px thumbnail
curl "http://localhost:8080/api/v1/thumbnails?path=%2Fphotos%2Fimage.jpg&size=512" \
  --output thumbnail_512.jpg
```

### DELETE /api/v1/thumbnails/cleanup

Clean up stale or orphaned thumbnails.

**Query Parameters:**

- `check_source` (optional): If `true`, removes thumbnails whose source files no longer exist

**Response:**

```json
{
  "removed_count": 15,
  "freed_bytes": 524288
}
```

**Example:**

```bash
curl -X DELETE "http://localhost:8080/api/v1/thumbnails/cleanup?check_source=true"
```

## Architecture

### Storage Strategy

**File System (cache/thumbnails/):**

- Actual JPEG thumbnail files
- Hash-based filenames: `<source_hash>_<size>.jpg`
- Managed by `DiskCache` with configurable size limits
- FIFO eviction when cache full

**Database (thumbnail_cache table):**

- Metadata and path mappings only (no BLOBs!)
- Tracks source file modification time for invalidation
- Composite unique constraint on (source_path, thumbnail_size)
- Supports multiple sizes per source file

### Generation Flow

```
1. Request → GET /api/v1/thumbnails?path=X&size=Y
2. Check database for cached entry
3. If cached AND source file unchanged:
   → Stream cached thumbnail from disk
4. If not cached OR source file modified:
   → Acquire generation lock (prevent duplicate work)
   → Check if file is RAW format (ARW, CR2, NEF, DNG, etc.)
   → If RAW AND transcoding enabled:
      • Transcode RAW → TIFF (temporary file in cache/disk/)
      • Generate thumbnail from TIFF
      • Clean up temporary TIFF
   → If not RAW OR transcoding disabled:
      • Generate thumbnail directly using OpenCV
   → Save JPEG to cache/thumbnails/
   → Update database with metadata
   → Stream thumbnail
5. Update last_accessed_at timestamp
```

### Thread Safety & Concurrency

- **Per-path generation locks**: Prevents duplicate generation when multiple requests for same thumbnail arrive
- **HTTP thread pool**: Thumbnail generation runs synchronously in HTTP server threads (configurable via `server.http.threadPool.maxThreads`)
- **Synchronous processing**: Each HTTP thread handles one request end-to-end
- **Database connection pool**: Safe concurrent database access
- **DiskCache mutex**: Thread-safe file operations
- **OpenCV threading disabled**: `cv::setNumThreads(0)` prevents internal thread spawning

### Cache Invalidation

Thumbnails are automatically invalidated when:

- Source file's `modified_at` timestamp changes
- Source file is deleted (cleanup endpoint)
- Cache size limit exceeded (FIFO eviction)

## Configuration

All configuration keys support runtime updates via the observable config system.

### Thumbnail Cache

```yaml
cache.thumbnail.location: cache/thumbnails # Cache directory
cache.thumbnail.size_limit_mb: 512 # Max cache size
```

### Thumbnail Generation

```yaml
thumbnail.default.size: 256 # Default size if not specified
thumbnail.allowed.sizes: "128,256,512,1024" # Comma-separated valid sizes
thumbnail.generation.timeoutMs: 5000 # Generation timeout
thumbnail.jpeg.quality: 85 # JPEG quality (0-100)
```

### RAW File Transcoding

```yaml
media.image.transcoding.enabled: true # Enable/disable RAW transcoding
media.image.transcoding.timeoutMs: 60000 # Transcoding timeout (60 seconds)
media.image.transcoding.preserveMetadata: true # Preserve EXIF metadata
```

**Note**: When `media.image.transcoding.enabled` is `false`, requests for RAW file thumbnails will return 500 error.

### HTTP Thread Pool

```yaml
server.http.threadPool.maxThreads: 8 # HTTP server thread pool (default: 2, recommended: 8-16)
server.http.threadPool.maxQueued: 50 # Request queue depth (default: 10, recommended: 50-100)
```

**Note:** Thumbnail generation runs synchronously in HTTP threads. Increase `maxThreads` for better concurrent thumbnail request handling.

## Database Schema

```sql
CREATE TABLE thumbnail_cache (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    source_path TEXT NOT NULL,
    cached_path TEXT NOT NULL,
    thumbnail_size INTEGER NOT NULL,
    file_size_bytes INTEGER NOT NULL,
    source_modified_at INTEGER NOT NULL,  -- Unix timestamp
    created_at INTEGER NOT NULL,          -- Unix timestamp
    last_accessed_at INTEGER NOT NULL,    -- Unix timestamp
    UNIQUE(source_path, thumbnail_size)   -- Allow multiple sizes per source
);

CREATE INDEX idx_thumbnail_source_path ON thumbnail_cache(source_path);
CREATE INDEX idx_thumbnail_source_modified ON thumbnail_cache(source_modified_at);
CREATE INDEX idx_thumbnail_last_accessed ON thumbnail_cache(last_accessed_at);
```

## Implementation Details

### Components

| Component                 | File                                            | Purpose                                              |
| ------------------------- | ----------------------------------------------- | ---------------------------------------------------- |
| `ThumbnailGenerator`      | `src/utils/thumbnail_generator.cpp`             | OpenCV-based thumbnail generation                    |
| `ThumbnailCacheOps`       | `src/database/thumbnail_cache_ops.cpp`          | Database operations                                  |
| `ThumbnailHandler`        | `src/core/webserver/web_handlers_thumbnail.cpp` | HTTP request handler                                 |
| `ThumbnailCleanupHandler` | `src/core/webserver/web_handlers_thumbnail.cpp` | Cleanup endpoint handler                             |
| `DiskCache`               | `src/filesmanager/disk_cache.cpp`               | Generic disk cache (extended for multiple instances) |

### DiskCache Extension

The `DiskCache` class was extended to support multiple cache instances with different configurations:

```cpp
// Transcoding cache (existing)
auto transcoding_cache = std::make_shared<DiskCache>(config, "cache.disk");

// Thumbnail cache (new)
auto thumbnail_cache = std::make_shared<DiskCache>(config, "cache.thumbnail");
```

Each instance reads its own configuration keys based on the prefix:

- `cache.disk.location` → Transcoding cache
- `cache.thumbnail.location` → Thumbnail cache

## Performance Characteristics

- **First request**: ~50-200ms (generation + save + stream)
- **Cached requests**: ~5-20ms (database lookup + file stream)
- **RAW file first request**: ~200-500ms (includes transcoding)
- **Concurrent requests**: Limited by HTTP thread pool (default: 8 threads)
- **Memory usage**: Minimal - images released immediately after processing
- **Disk usage**: Controlled via `cache.thumbnail.size_limit_mb`

**Concurrency:**

- Each HTTP thread processes one thumbnail request at a time
- With 8 threads: Can handle 8 concurrent thumbnail generations
- Additional requests queue up (max 50 in queue)
- Cache hits return immediately without blocking threads

## Testing

Example test scenarios:

```bash
# Test generation
curl "http://localhost:8080/api/v1/thumbnails?path=/photos/img.jpg&size=256"

# Test cache hit (should be instant)
curl "http://localhost:8080/api/v1/thumbnails?path=/photos/img.jpg&size=256"

# Test multiple sizes
curl "http://localhost:8080/api/v1/thumbnails?path=/photos/img.jpg&size=128"
curl "http://localhost:8080/api/v1/thumbnails?path=/photos/img.jpg&size=512"

# Test invalid size
curl "http://localhost:8080/api/v1/thumbnails?path=/photos/img.jpg&size=300"
# Returns: 400 Bad Request

# Test missing file
curl "http://localhost:8080/api/v1/thumbnails?path=/nonexistent.jpg&size=256"
# Returns: 404 Not Found

# Test RAW file thumbnail
curl "http://localhost:8080/api/v1/thumbnails?path=/photos/IMG_1234.arw&size=512" \
  --output raw_thumbnail.jpg
# Automatically transcodes ARW → TIFF → thumbnail

# Test with transcoding disabled
# Set media.image.transcoding.enabled: false in config
curl "http://localhost:8080/api/v1/thumbnails?path=/photos/IMG_1234.cr2&size=256"
# Returns: 500 Internal Server Error
```

## Known Limitations

1. **Supported Formats**: Limited to what ImageMagick and OpenCV can read
   - RAW files: ARW, CR2, NEF, DNG (transcoded automatically)
   - Standard formats: JPEG, PNG, TIFF, BMP, WebP, GIF
2. **Cache Persistence**: Cache survives server restarts
3. **Transcoding Performance**: RAW file thumbnails take longer (~50-200ms extra for transcoding)
4. **Disk Usage**: Transcoding temporarily uses `cache.disk` space (cleaned up after thumbnail generation)

## Future Enhancements

- Background thumbnail generation for all processed files
- WebP format support for smaller file sizes
- Progressive JPEG for better loading experience
- Automatic cache warmup on startup
- Thumbnail sprites for gallery views
