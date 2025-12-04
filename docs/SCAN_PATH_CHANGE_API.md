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


