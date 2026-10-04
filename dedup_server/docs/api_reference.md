# Duplicate Groups API

## Overview

REST API endpoint for retrieving paginated duplicate groups with their representative and candidate files (all as fully qualified paths).

## Endpoint

```
GET /api/v1/duplicates/groups
```

## Query Parameters

| Parameter | Type    | Required | Default | Max  | Description                                   |
| --------- | ------- | -------- | ------- | ---- | --------------------------------------------- |
| `start`   | integer | No       | 0       | -    | Starting index (0-based offset)               |
| `limit`   | integer | No       | 100     | 1000 | Number of groups to return per page           |
| `mode`    | string  | No       | (all)   | -    | Filter by detection mode: `FAST` or `QUALITY` |

### Alternative Parameter: `end`

Instead of `limit`, you can provide `end` to specify the ending index:

```
GET /api/v1/duplicates/groups?start=10&end=20
```

This will return groups from index 10 to 20 (limit = end - start = 10 groups).

## Response Format

```json
{
  "groups": [
    {
      "id": 1,
      "mode": "FAST",
      "representative": "/Users/vinaysharma/Pictures/IMG_001.jpg",
      "candidates": [
        "/Users/vinaysharma/Pictures/IMG_001_copy.jpg",
        "/Users/vinaysharma/Pictures/Backup/IMG_001.jpg"
      ],
      "similarity_threshold": 0.95,
      "member_count": 3,
      "created_at": "2025-10-18 10:30:45",
      "updated_at": "2025-10-18 10:31:20"
    },
    {
      "id": 2,
      "mode": "FAST",
      "representative": "/Users/vinaysharma/Pictures/DSC_7890.jpg",
      "candidates": ["/Users/vinaysharma/Pictures/DSC_7890_edited.jpg"],
      "similarity_threshold": 0.95,
      "member_count": 2,
      "created_at": "2025-10-18 10:32:15",
      "updated_at": "2025-10-18 10:32:15"
    }
  ],
  "total_count": 1523,
  "start": 0,
  "end": 100,
  "returned": 2
}
```

## Response Fields

### Root Object

| Field         | Type    | Description                                       |
| ------------- | ------- | ------------------------------------------------- |
| `groups`      | array   | Array of duplicate group objects (see below)      |
| `total_count` | integer | Total number of duplicate groups in the database  |
| `start`       | integer | Starting index of this page (from request)        |
| `end`         | integer | Ending index (start + limit)                      |
| `returned`    | integer | Actual number of groups returned in this response |

### Group Object

| Field                  | Type    | Description                                                         |
| ---------------------- | ------- | ------------------------------------------------------------------- |
| `id`                   | integer | Unique duplicate group ID                                           |
| `mode`                 | string  | Detection mode: `"FAST"` or `"QUALITY"`                             |
| `representative`       | string  | Fully qualified path to the representative (best) file              |
| `candidates`           | array   | Array of fully qualified paths to duplicate candidate files         |
| `similarity_threshold` | number  | Similarity threshold used for this group (e.g., 0.95)               |
| `member_count`         | integer | Total number of members in this group (representative + candidates) |
| `created_at`           | string  | ISO 8601 timestamp when the group was first created                 |
| `updated_at`           | string  | ISO 8601 timestamp when the group was last modified                 |

## Sorting

Results are **always sorted by `created_at` in ascending order** (oldest groups first).

This ensures consistent pagination and allows clients to:

- Get newest groups by requesting the last page
- Process groups chronologically
- Resume processing from a specific creation time

## Database Optimization

An index on `duplicate_groups(created_at)` ensures efficient pagination:

```sql
CREATE INDEX IF NOT EXISTS idx_duplicate_groups_created_at
ON duplicate_groups(created_at);
```

## Examples

### Get first 10 groups (all modes)

```bash
curl "http://localhost:8080/api/v1/duplicates/groups?start=0&limit=10"
```

### Get first 10 FAST mode groups only

```bash
curl "http://localhost:8080/api/v1/duplicates/groups?start=0&limit=10&mode=FAST"
```

### Get first 10 QUALITY mode groups only

```bash
curl "http://localhost:8080/api/v1/duplicates/groups?start=0&limit=10&mode=QUALITY"
```

### Get groups 100-200

```bash
curl "http://localhost:8080/api/v1/duplicates/groups?start=100&limit=100"
```

### Get groups using end parameter

```bash
curl "http://localhost:8080/api/v1/duplicates/groups?start=50&end=75"
# Returns 25 groups (from index 50 to 74)
```

### Get maximum allowed page (1000 groups)

```bash
curl "http://localhost:8080/api/v1/duplicates/groups?start=0&limit=1000"
```

## Error Responses

### 500 Internal Server Error

```json
{
  "error": "Internal server error",
  "message": "Failed to retrieve duplicate groups: ..."
}
```

## Representative vs Candidates

- **Representative**: The "best" file in the group (selected by file size and age)
- **Candidates**: All other duplicate files in the group
- The representative is **not included** in the `candidates` array
- `member_count` = 1 (representative) + number of candidates

## Use Cases

### Bulk Review

```bash
# Get all groups in batches of 100
for i in {0..1500..100}; do
  curl "http://localhost:8080/api/v1/duplicates/groups?start=$i&limit=100" \
    | jq '.groups[] | {id, representative, count: (.candidates | length)}'
done
```

### Find Specific Group Range

```bash
# Get groups created in a specific range
curl "http://localhost:8080/api/v1/duplicates/groups?start=1000&limit=500" \
  | jq '.groups[] | select(.mode == "QUALITY")'
```

### Extract All Duplicate Paths

```bash
# Get all duplicate candidates (excluding representatives)
curl "http://localhost:8080/api/v1/duplicates/groups?start=0&limit=1000" \
  | jq -r '.groups[].candidates[]'
```

### Count Total Duplicates Without Fetching Data

```bash
# Get just the metadata
curl "http://localhost:8080/api/v1/duplicates/groups?start=0&limit=1" \
  | jq '{total_groups: .total_count, this_page_returned: .returned}'
```

## Performance Characteristics

- **Query Time**: < 50ms for 100 groups (with members) using indexed `created_at`
- **Response Size**: ~500 bytes per group (varies with path lengths and candidate count)
- **Typical Page Size**: 100 groups = ~50KB response
- **Maximum Page Size**: 1000 groups = ~500KB response

## Implementation Details

- **Database**: Single query for groups + individual queries for members (N+1 pattern)
- **Indexing**: `idx_duplicate_groups_created_at` ensures fast pagination
- **Thread Safety**: Read-only operations using database session pool
- **Caching**: No caching (always returns fresh data from database)

## Future Enhancements

Potential improvements for consideration:

1. **Filtering**: Add query parameters for mode, date range, member count
2. **Sorting Options**: Allow sorting by updated_at, member_count, similarity
3. **Batch Operations**: Add endpoints for bulk actions (delete, review, keep)
4. **Streaming**: Support streaming large result sets with chunked encoding
5. **Metadata**: Include file sizes, creation dates, similarity scores in response

---

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
- Hash-based filenames: `<source_hash>_<size>_r<render_revision>.jpg`
  (`ThumbnailCacheOps::cacheFileName`; revision is 0 until non-destructive
  edits exist — THUMB-1)
- Managed by `DiskCache` with configurable size limits
- FIFO eviction when cache full

**Database (thumbnail_cache table):**

- Metadata and path mappings only (no BLOBs!)
- Tracks source file modification time for invalidation
- Composite unique constraint on (source_path, thumbnail_size, render_revision)
- Supports multiple sizes (and, later, revisions) per source file
- Thumbnails are generated with `-resize NxN` (fit-inside-box, aspect
  preserved) — the client lays tiles out at the image's aspect ratio (THUMB-1),
  so this must never become a fill/crop form (`test_magick_args.cpp` guards it)

**List RPCs (`DuplicateFile.display_width/height`, `DayFile.display_width/height`):**

- THUMB-1: the image's display geometry (auto-oriented base dimensions today,
  from `scanned_files.exif_resolution` via `computeDisplayDimensions`), so the
  client can reserve the correct tile width before the bitmap arrives. `0`
  means unknown → square tile.

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
    render_revision INTEGER NOT NULL DEFAULT 0,  -- THUMB-1: 0 = no user edits; part of the asset identity
    file_size_bytes INTEGER NOT NULL,
    source_modified_at INTEGER NOT NULL,  -- Unix timestamp
    created_at INTEGER NOT NULL,          -- Unix timestamp
    last_accessed_at INTEGER NOT NULL,    -- Unix timestamp
    UNIQUE(source_path, thumbnail_size, render_revision)  -- multiple sizes AND revisions per source
);

CREATE INDEX idx_thumbnail_source_path ON thumbnail_cache(source_path);
CREATE INDEX idx_thumbnail_source_modified ON thumbnail_cache(source_modified_at);
CREATE INDEX idx_thumbnail_last_accessed ON thumbnail_cache(last_accessed_at);
```

`render_revision` (THUMB-1) is
the seam for the future non-destructive edit pipeline: every keyed statement
(`kUpsertThumbnailCache`'s `ON CONFLICT` target included), the on-disk filename
(`<hash>_<size>_r<revision>.jpg`, `ThumbnailCacheOps::cacheFileName`) and the
best-fit subquery filter on it. `ThumbnailCacheOps::ensureTable` detects a
pre-revision table (`PRAGMA table_info` lacks the column) and drops/recreates it
— for a cache table, a schema change legitimately invalidates the cache.

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

## Performance Optimization

### Under Normal Load

- **First request**: ~50-200ms (generation + save + stream)
- **Cached requests**: ~5-20ms (database lookup + file stream)
- **RAW file first request**: ~200-500ms (includes transcoding)

### Under Heavy Load

When the server is processing many media files concurrently, thumbnail API response times may increase. The primary bottleneck is typically database contention.

**Recommended Solution: Enable WAL Mode**

SQLite's Write-Ahead Logging (WAL) mode allows concurrent reads and writes, dramatically improving performance under load:

```sql
PRAGMA journal_mode = WAL;
PRAGMA synchronous = NORMAL;
PRAGMA cache_size = -20000;  -- 20MB cache
```

**Expected Improvement:**
- Cache hits: 500-1200ms → 20-50ms (25-60x faster)
- First generation: 1000-2000ms → 100-300ms

**Additional Optimizations:**
- Increase database session pool: `database.session.poolMax: 32`
- Increase SQLite cache size for faster queries

### Performance Characteristics by Load

| Load Level | CPU Usage | Cache Hit | First Generation | RAW First Gen |
|------------|-----------|-----------|------------------|---------------|
| **Normal** (<50%) | Low | 5-20ms | 50-200ms | 200-500ms |
| **Heavy** (70-90%) | High | 50-150ms | 200-500ms | 1000-3000ms |
| **Extreme** (>90%) | Saturated | 500-1200ms | 1000-2000ms | 4000-8000ms |

**Note:** With WAL mode enabled, even under heavy load, cache hits should remain under 50ms.

## Troubleshooting

### ARW File Failures Under Heavy Load

**Symptoms:**
- Some ARW files return HTTP 500 during bulk thumbnail generation
- Failures are inconsistent and appear under heavy system load
- Manual retry succeeds when system load is lower

**Root Cause:**
Timeout issues when server is under extreme CPU saturation (>90%). ARW transcoding can take 4-10 seconds under extreme load, exceeding script timeouts.

**Solutions:**

1. **Increase Script Timeout** (if using bulk generation scripts):
   ```bash
   curl --max-time 30 ...  # Increase from 10 to 30 seconds
   ```

2. **Add Rate Limiting** (prevent overwhelming server):
   ```bash
   sleep 0.2  # 200ms between requests in bulk scripts
   ```

3. **Increase Database Session Timeout**:
   ```yaml
   database.session.acquireTimeoutMs: 10000  # 10 seconds (up from 3)
   ```

4. **Enable WAL Mode** (see Performance Optimization section above)

**Verification:**
```bash
# Test individual ARW files
curl -w "Time: %{time_total}s\n" \
  "http://localhost:8080/api/v1/thumbnails?path=/path/to/file.ARW&size=256" \
  -o thumb.jpg
```

### Slow Response Times Under Load

**Symptoms:**
- Thumbnail API takes 500-1200ms even for cached thumbnails
- System is processing many media files concurrently

**Root Cause:**
SQLite DELETE journal mode causes thumbnail reads to wait for media processing writes to complete.

**Solution:**
Enable WAL mode (see Performance Optimization section). This is the most impactful fix and should reduce response times to 20-50ms even under heavy load.

### Database Session Contention

**Symptoms:**
- "Timed out acquiring DB session" errors
- High number of concurrent threads competing for database sessions

**Solution:**
Increase the database session pool to match or exceed the number of competing threads:
```yaml
database.session.poolMax: 32  # Increase from default 20
```

**Thread Count:**
- Media processors: Typically 14 threads
- HTTP threads: Typically 8 threads
- Total: 22+ threads competing for sessions

## Future Enhancements

- Background thumbnail generation for all processed files
- WebP format support for smaller file sizes
- Progressive JPEG for better loading experience
- Automatic cache warmup on startup
- Thumbnail sprites for gallery views

---

# Scan Path Change API

## Overview

The Scan Path Change API allows you to update registered media location paths when external drives are remounted at different locations. This is particularly useful when:

- External drives are reconnected and assigned different mount points
- Network shares are remapped to different paths
- Media files are moved to a new location while maintaining the same directory structure

The API automatically verifies the new path by sampling files and updates all associated file paths across the entire database.

## API Endpoint

**POST** `/api/v1/media-locations/change-path`

## Request

### Headers
```
Content-Type: application/json
```

### Request Body

```json
{
  "old_path": "/Volumes/OldDrive/Photos",
  "new_path": "/Volumes/NewDrive/Photos",
  "sample_size": 20
}
```

### Parameters

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `old_path` | string | Yes | - | The currently registered scan path that needs to be changed |
| `new_path` | string | Yes | - | The new scan path where files are now located |
| `sample_size` | integer | No | 20 | Number of files to randomly sample for verification (1-100) |

## Response

### Success Response (200 OK)

```json
{
  "status": "ok",
  "old_path": "/Volumes/OldDrive/Photos",
  "new_path": "/Volumes/NewDrive/Photos",
  "files_verified": 20,
  "files_verified_success": 20,
  "total_files": 1500,
  "files_updated": 1500,
  "files_failed": 0,
  "verification_success_rate": 1.0,
  "update_details": {
    "scanned_files": {"updated": 1500, "failed": 0},
    "image_artifacts": {"updated": 1200, "failed": 0},
    "processing_errors": {"updated": 50, "failed": 0},
    "duplicate_groups": {"updated": 30, "failed": 0},
    "duplicate_members": {"updated": 200, "failed": 0},
    "thumbnail_cache": {"updated": 800, "failed": 0}
  }
}
```

### Partial Success Response (200 OK)

When some files fail to update but verification passed:

```json
{
  "status": "partial_success",
  "old_path": "/Volumes/OldDrive/Photos",
  "new_path": "/Volumes/NewDrive/Photos",
  "files_verified": 20,
  "files_verified_success": 20,
  "total_files": 1500,
  "files_updated": 1450,
  "files_failed": 50,
  "verification_success_rate": 1.0,
  "update_details": {
    "scanned_files": {"updated": 1450, "failed": 50},
    "image_artifacts": {"updated": 1200, "failed": 0},
    "duplicate_groups": {"updated": 30, "failed": 0}
  },
  "error": "Some files failed to update: 50 failed"
}
```

### Error Responses

#### 400 Bad Request - Verification Failed

```json
{
  "status": "error",
  "error": "Verification failed: only 15 out of 20 files verified successfully (75% < 80% threshold)"
}
```

#### 400 Bad Request - Invalid Input

```json
{
  "status": "error",
  "error": "New path does not exist or is not a directory"
}
```

#### 404 Not Found - Old Path Not Registered

```json
{
  "status": "error",
  "error": "Old path is not registered"
}
```

#### 404 Not Found - No Files Found

```json
{
  "status": "error",
  "error": "No files found for the old location"
}
```

#### 500 Internal Server Error

```json
{
  "status": "error",
  "error": "Database update failed"
}
```

## How It Works

### 1. Verification Process

Before updating any paths, the API performs verification:

1. **Query Files**: Retrieves all files associated with the old location using the `location_key`
2. **Random Sampling**: Randomly selects files (default: 20) for verification
3. **File Verification**: For each sampled file:
   - Constructs the new file path using the relative path
   - Verifies the file exists at the new location
   - Verifies the file size matches the stored size
4. **Success Threshold**: Requires at least 80% of sampled files to verify successfully
5. **Abort on Failure**: If verification fails, no database changes are made

### 2. Path Update Process

If verification succeeds:

1. **Location Key Update**:
   - Creates new `mediaLocation:<new_hash>` entry in user_settings
   - Updates all `scanned_files.location_key` from old to new
   - Deletes old `mediaLocation:<old_hash>` entry

2. **File Path Updates**: For each file:
   - Reconstructs relative path if needed
   - Updates `file_path` in all tables:
     - `scanned_files`
     - `image_artifacts`
     - `processing_errors`
     - `duplicate_groups` (representative_file_path)
     - `duplicate_members`
     - `thumbnail_cache` (source_path)
   - Updates `relative_path` in scanned_files

3. **Partial Failure Handling**: If some file updates fail, the operation continues and reports partial success

## Usage Examples

### Example 1: Basic Path Change

**Scenario**: External drive was remounted from `/Volumes/MyDrive` to `/Volumes/MyDrive2`

```bash
curl -X POST http://localhost:8080/api/v1/media-locations/change-path \
  -H "Content-Type: application/json" \
  -d '{
    "old_path": "/Volumes/MyDrive",
    "new_path": "/Volumes/MyDrive2"
  }'
```

### Example 2: Custom Sample Size

**Scenario**: Large media library, want to verify more files before updating

```bash
curl -X POST http://localhost:8080/api/v1/media-locations/change-path \
  -H "Content-Type: application/json" \
  -d '{
    "old_path": "/Volumes/OldDrive/Photos",
    "new_path": "/Volumes/NewDrive/Photos",
    "sample_size": 50
  }'
```

### Example 3: Using Swagger UI

1. Navigate to `http://localhost:8080/` in your browser
2. Find the `/api/v1/media-locations/change-path` endpoint
3. Click "Try it out"
4. Enter the request body:
   ```json
   {
     "old_path": "/Volumes/OldDrive/Photos",
     "new_path": "/Volumes/NewDrive/Photos"
   }
   ```
5. Click "Execute"
6. Review the response

### Example 4: JavaScript/TypeScript

```javascript
async function changeScanPath(oldPath, newPath) {
  const response = await fetch('http://localhost:8080/api/v1/media-locations/change-path', {
    method: 'POST',
    headers: {
      'Content-Type': 'application/json',
    },
    body: JSON.stringify({
      old_path: oldPath,
      new_path: newPath,
      sample_size: 20
    })
  });

  const result = await response.json();
  
  if (result.status === 'ok') {
    console.log(`Successfully updated ${result.files_updated} files`);
    console.log(`Verification rate: ${(result.verification_success_rate * 100).toFixed(1)}%`);
  } else {
    console.error('Error:', result.error);
  }
  
  return result;
}

// Usage
changeScanPath('/Volumes/OldDrive/Photos', '/Volumes/NewDrive/Photos');
```

## Important Notes

### Prerequisites

1. **Old Path Must Be Registered**: The old path must be a currently registered media location
2. **New Path Must Exist**: The new path must exist and be a valid directory
3. **File Structure Must Match**: Files should maintain the same relative structure under the new path
4. **Files Must Be Accessible**: The server must have read access to files at the new location

### Verification Requirements

- **Minimum Success Rate**: 80% of sampled files must verify successfully
- **File Size Matching**: Files are verified for both existence and size matching
- **Random Sampling**: Files are randomly selected to ensure representative verification

### What Gets Updated

The API updates file paths in the following database tables:

1. **scanned_files**: Main file registry
   - `file_path`: Updated to new path
   - `relative_path`: Reconstructed if needed
   - `location_key`: Updated to new location key

2. **image_artifacts**: Image processing artifacts
   - `file_path`: Updated to new path

3. **processing_errors**: Error records
   - `file_path`: Updated to new path

4. **duplicate_groups**: Duplicate detection groups
   - `representative_file_path`: Updated to new path

5. **duplicate_members**: Duplicate group members
   - `file_path`: Updated to new path

6. **thumbnail_cache**: Thumbnail cache entries
   - `source_path`: Updated to new path

7. **user_settings**: Media location registry
   - Old `mediaLocation:<hash>` entry deleted
   - New `mediaLocation:<hash>` entry created

### Error Handling

- **Verification Failure**: If verification fails (below 80% success rate), no database changes are made
- **Partial Updates**: If some file updates fail after verification passes, the operation continues and reports partial success
- **Transaction Safety**: Updates are performed with per-table transactions for safety

## Troubleshooting

### Verification Fails

**Problem**: Verification fails with message "only X out of Y files verified successfully"

**Possible Causes**:
- Files don't exist at the new path
- File sizes don't match (files may have been modified)
- Incorrect new path specified
- Permission issues accessing files at new location

**Solutions**:
1. Verify the new path is correct
2. Ensure all files exist at the new location
3. Check file permissions
4. Verify file sizes haven't changed

### Old Path Not Registered

**Problem**: Error "Old path is not registered"

**Solution**: 
1. Check registered locations: `GET /api/v1/media-locations`
2. Verify the exact path (case-sensitive, trailing slashes matter)
3. Register the old path first if needed: `POST /api/v1/media-locations/register`

### New Path Doesn't Exist

**Problem**: Error "New path does not exist or is not a directory"

**Solution**:
1. Verify the new path exists
2. Ensure it's a directory, not a file
3. Check path spelling and case sensitivity

### Partial Success

**Problem**: Some files fail to update (partial_success status)

**Possible Causes**:
- Database constraints violations
- Concurrent modifications
- File paths that don't match expected format

**Solution**:
- Review the `update_details` in the response
- Check logs for specific error messages
- Retry the operation if needed

## Best Practices

1. **Backup First**: Consider backing up your database before running path changes on large libraries
2. **Verify Files First**: Manually verify a few files exist at the new location before using the API
3. **Use Appropriate Sample Size**: For large libraries, consider increasing `sample_size` for better verification
4. **Monitor Results**: Check the `update_details` to ensure all tables were updated correctly
5. **Test with Small Set**: Test the API with a small media location first to understand the behavior

## Related Endpoints

- `POST /api/v1/media-locations/register` - Register a new media location
- `POST /api/v1/media-locations/deregister` - Deregister a media location
- `GET /api/v1/media-locations` - List all registered media locations (via user-settings endpoint)

## Implementation Details

For technical details about the implementation, see:
- Design Document: `docs/SCAN_PATH_CHANGE_DESIGN.md`
- Source Code:
  - Service: `src/filesmanager/files_service.cpp`
  - Handler: `src/core/webserver/web_handlers_user_settings.cpp`
  - Database: `src/database/scanned_files/scanned_files_ops.cpp`

---

# Media Deduplication Server Web API

This document describes the web server component of the Media Deduplication Server, which provides a RESTful HTTP API for configuration management.

## Overview

The web server integrates with the unified observable configuration system and now serves static content directly from disk to provide:

- **HTTP endpoints** for configuration management
- **Real-time configuration updates** via the unified observable system
- **OpenAPI specification** for API documentation
- **CORS support** for web applications
- **JSON-based communication** for easy integration

## Architecture

```
┌─────────────────┐    HTTP Requests    ┌──────────────────┐    Configuration    ┌─────────────────────┐
│   HTTP Client   │ ──────────────────► │   Web Server     │ ──────────────────► │ Unified Observable  │
│                 │                     │                  │                     │ Config Manager      │
└─────────────────┘                     └──────────────────┘                     └─────────────────────┘
                                                │
                                                ▼
                                        ┌──────────────────┐
                                        │ Configuration    │
                                        │ File (YAML)      │
                                        └──────────────────┘
```

## Features

### 🔄 **Bidirectional Configuration Updates**

- **HTTP API** → **Configuration File**: Updates via PUT requests are automatically saved
- **Configuration File** → **HTTP API**: File changes are automatically detected and loaded
- **Real-time synchronization** between API, memory, and file system

### 🌐 **RESTful API Design**

- **Standard HTTP methods** (GET, PUT, POST)
- **Consistent response format** with proper HTTP status codes
- **JSON request/response bodies** for easy integration

### 📚 **OpenAPI Documentation (Static)**

- **Static OpenAPI spec** served from `src/core/webserver/static/api/openapi.json`
- **Interactive documentation** can be generated using Swagger UI
- **Complete endpoint specifications** with request/response schemas

### 🔒 **CORS Support**

- **Cross-origin requests** supported for web applications
- **Configurable CORS headers** for security
- **Preflight OPTIONS requests** handled automatically

## API Endpoints

### 1. **GET /api/v1/config**

Retrieves all configuration properties.

**Response:**

```json
{
  "server.host": "0.0.0.0",
  "server.port": 8080,
  "logging.level": "info",
  "database.path": "data/dedup_server.db"
}
```

### 2. **GET /api/v1/config/{key}**

Retrieves a specific configuration property.

**Parameters:**

- `key` (path): Configuration property key

**Response:**

```json
{
  "key": "logging.level",
  "value": "info",
  "description": "Logging level",
  "modified": false
}
```

### 3. **PUT /api/v1/config/{key}**

Updates a configuration property.

**Parameters:**

- `key` (path): Configuration property key

**Request Body:**

```json
{
  "value": "debug"
}
```

**Response:**

```json
{
  "message": "Property updated successfully",
  "key": "logging.level",
  "value": "debug"
}
```

### 4. **POST /api/v1/config/reload**

Reloads configuration from the file system.

**Response:**

```json
{
  "message": "Configuration reloaded successfully"
}
```

### 5. **GET /api/v1/config/status**

Retrieves the configuration system status.

**Response:**

```json
{
  "valid": true,
  "config_file": "config/config.yaml",
  "property_count": 8,
  "validation_errors": []
}
```

### 6. **GET /api/openapi.json**

Retrieves the OpenAPI 3.0 specification (served as a static JSON file).

**Response:** Complete OpenAPI specification in JSON format.

### 7. **GET /api/v1/tpm/status**

Retrieves the Thread Pool Manager status.

**Response:**

```json
{
  "max_threads": 15,
  "current_threads": 3,
  "running_tasks": 2,
  "queued_tasks": 0,
  "shutdown_in_progress": false
}
```

### 8. **User Settings API**

CRUD for simple key/value user settings persisted in SQLite.

#### GET /api/v1/user-settings

Returns all settings as a flat object.

**Response:**

```json
{
  "setting1": "value1",
  "setting2": "value2"
}
```

#### GET /api/v1/user-settings/{key}

Returns a single setting by key.

**Response:**

```json
{
  "key": "setting1",
  "value": "value1"
}
```

#### PUT /api/v1/user-settings/{key}

Create or update a setting.

**Request body:**

```json
{ "value": "string" }
```

**Response:**

```json
{
  "message": "Setting updated successfully",
  "key": "setting1",
  "value": "new_value"
}
```

#### DELETE /api/v1/user-settings/{key}

Deletes a setting by key.

**Response:**

```json
{
  "message": "Setting deleted successfully",
  "key": "setting1"
}
```

### 9. **Media Locations API**

Register and deregister media library directories.

#### POST /api/v1/media-locations/register

**Request body:**

```json
{ "directory": "/path/to/media" }
```

**Response:**

```json
{ "status": "ok", "directory": "/path/to/media" }
```

#### POST /api/v1/media-locations/deregister

**Request body:**

```json
{ "directory": "/path/to/media" }
```

**Response:**

```json
{ "status": "ok", "directory": "/path/to/media" }
```

## Configuration Reference

For the authoritative configuration keys, defaults, and live update behavior, see `config/CONFIGURATION_REFERENCE.md`.

## Usage Examples

### **Starting the Server**

```bash
# Build the project
./build.sh

# Start the server
./build/bin/media_dedup_server
```

### **Testing the API**

```bash
# Run the full demo (starts server and tests all endpoints)
./demo_web_server.sh

# Start server only
./demo_web_server.sh --start

# Test API endpoints only (server must be running)
./demo_web_server.sh --test
```

### **Using curl**

```bash
# Get all configuration
curl http://localhost:8080/api/v1/config

# Get specific property
curl http://localhost:8080/api/v1/config/logging.level

# Update property
curl -X PUT http://localhost:8080/api/v1/config/logging.level \
  -H "Content-Type: application/json" \
  -d '{"value": "debug"}'

# Get system status
curl http://localhost:8080/api/v1/config/status

# Get OpenAPI spec
curl http://localhost:8080/api/openapi.json
```

### **Using JavaScript/Fetch**

```javascript
// Get all configuration
const response = await fetch("http://localhost:8080/api/v1/config");
const config = await response.json();

// Update property
const updateResponse = await fetch(
  "http://localhost:8080/api/v1/config/logging.level",
  {
    method: "PUT",
    headers: {
      "Content-Type": "application/json",
    },
    body: JSON.stringify({ value: "debug" }),
  }
);

// Get property details
const propertyResponse = await fetch(
  "http://localhost:8080/api/v1/config/server.port"
);
const property = await propertyResponse.json();
```

## Integration with Unified Observable System

The web server is tightly integrated with the unified observable configuration system:

### **Automatic Property Creation**

- New properties added to the configuration file are automatically discovered
- Properties can be created via API calls and immediately persisted
- All property changes trigger appropriate events and notifications

### **Real-time Updates**

- Configuration file changes are automatically detected and loaded
- API updates are immediately reflected in the file system
- Bidirectional synchronization prevents data loss

### **Event-driven Architecture**

- Configuration changes emit events for other system components
- File system changes trigger reload events
- Property updates notify all subscribers automatically

## Error Handling

The API provides comprehensive error handling:

### **HTTP Status Codes**

- `200 OK`: Request successful
- `400 Bad Request`: Invalid request (missing required fields, invalid data)
- `404 Not Found`: Configuration property not found
- `405 Method Not Allowed`: HTTP method not supported for endpoint
- `500 Internal Server Error`: Server-side error

### **Error Response Format**

```json
{
  "error": "Property not found: invalid.key",
  "status": 404
}
```

## Security Considerations

### **CORS Configuration**

- Currently allows all origins (`*`) for development
- Can be restricted to specific domains in production
- Supports preflight OPTIONS requests

### **Input Validation**

- All input is validated before processing
- JSON request bodies are parsed and validated
- Property keys are sanitized and validated

### **File System Access**

- Configuration files are read from predefined paths
- File system operations are restricted to configuration directories
- Automatic backup and validation of configuration changes

## Performance

### **Efficient Request Handling**

- Lightweight HTTP request processing
- Minimal memory overhead per request
- Fast JSON serialization/deserialization

### **Configuration Caching**

- Configuration properties are cached in memory
- File system access is minimized
- Background file monitoring with configurable intervals

## Monitoring and Debugging

### **Server Logs**

- Comprehensive logging of all API requests
- Configuration change tracking
- Error logging with stack traces

### **Health Checks**

- `/api/v1/config/status` endpoint for health monitoring
- Configuration validation status
- Property count and validation error reporting

### **Debug Information**

- Detailed error messages for troubleshooting
- Configuration file path and status information
- Property modification tracking

## Future Enhancements

### **Planned Features**

- **Authentication and Authorization**: User-based access control
- **Rate Limiting**: API request throttling
- **Metrics Collection**: Request/response statistics
- **WebSocket Support**: Real-time configuration updates
- **Configuration Templates**: Predefined configuration sets

### **API Extensions**

- **Batch Operations**: Update multiple properties at once
- **Configuration History**: Track configuration changes over time
- **Configuration Validation**: Schema-based validation rules
- **Configuration Export/Import**: Backup and restore functionality

## Troubleshooting

### **Common Issues**

#### **Server Won't Start**

- Check if port 8080 is available
- Verify configuration file exists and is valid
- Check server logs for error messages

#### **API Requests Fail**

- Ensure server is running on expected host/port
- Verify endpoint URLs are correct
- Check CORS settings for web applications

#### **Configuration Not Persisting**

- Verify file write permissions
- Check configuration file path
- Ensure `triggerSave()` is called after updates

#### **File Monitoring Not Working**

- Check if file monitoring is enabled
- Verify file system supports change detection
- Check monitoring thread status

### **Debug Commands**

```bash
# Check server status
curl http://localhost:8080/api/v1/config/status

# View server logs
tail -f server.log

# Test specific endpoint
curl -v http://localhost:8080/api/v1/config

# Check configuration file
cat config/config.yaml
```

## Contributing

When adding new API endpoints or modifying existing ones:

1. **Update the OpenAPI specification** in `OpenApiSpecHandler`
2. **Add appropriate error handling** for new endpoints
3. **Include unit tests** for new functionality
4. **Update this documentation** with new endpoint details
5. **Follow the existing code style** and patterns

## License

This web server component is part of the Media Deduplication Server project and follows the same licensing terms.


---

# Adjustments (gRPC, ADJ-1)

Non-destructive per-image edits (crop + rotate) — `AdjustmentsService` in
`src/api/dedup.proto`, implemented by `include/core/grpc_server/adjustments_service.hpp`
over `Adjustments::AdjustmentStore`. Full design: `docs/todo_image_adjustments.md`.

| RPC | Purpose |
|---|---|
| `DescribeTools` | Tool descriptors (ids, labels, parameter limits/defaults) in application order; the client builds its controls from these |
| `GetAdjustments` | The file's document (sidecar JSON), revision, post-edit geometry, sidecar location/spool state |
| `SetAdjustments` | Validate + store a complete document; writes the sidecar (or spools it), invalidates cached renders, pushes `MediaReadyEvent{media_type="adjustments"}`. `INVALID_ARGUMENT` carries the tool's error; identity removes the edit |
| `GetEditsRoot` / `SetEditsRoot` | Per-location edit-file folder (default: the location); changing it moves existing sidecars; `ALREADY_EXISTS` when shared/nested with another location |

Related fields: `GetMediaRequest.render_revision` (informational) and
`MediaPathResponse.render_revision` (the revision actually served);
`DayFile`/`DuplicateFile.render_revision`; `RegisterLocationRequest.edits_root`;
`DeregisterLocationRequest.delete_adjustment_files`; `GetStatusResponse.edits_roots`.

Storage: `image_adjustments(file_path PK, location_key, render_revision, document,
sidecar_path, sidecar_mtime, updated_at)`; sidecars `<asset>.vurio.json`; spool
`<database dir>/pending_edits/<location sha>/<relative path>.vurio.json`.
Thumbnails are keyed by `render_revision`; the QHD preview is base pixels (the
viewer applies the chain itself); native renders carry the chain.
