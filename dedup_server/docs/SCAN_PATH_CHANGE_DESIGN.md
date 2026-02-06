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

