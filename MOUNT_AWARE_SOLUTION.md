# Mount-Aware Path Storage Solution

## Problem

The current dedup server stores absolute file paths in the database, which causes issues when SMB/NFS mounts change:

- **Before**: `/Volumes/truenas._smb._tcp.local/Video/...` (G share)
- **After**: `/Volumes/truenas._smb._tcp.local-1/Video/...` (B share)

When macOS reconnects to network drives, mount points can change, making stored paths invalid.

## Solution: Mount-Aware Path Storage

### Core Components

1. **MountManager** (`include/core/mount_manager.hpp`)

   - Detects current SMB/NFS mounts
   - Converts absolute paths to relative paths
   - Resolves relative paths back to absolute paths
   - Handles mount changes gracefully

2. **Enhanced Database Schema**
   ```sql
   CREATE TABLE scanned_files (
       id INTEGER PRIMARY KEY AUTOINCREMENT,
       file_path TEXT NOT NULL UNIQUE,           -- Original absolute path
       relative_path TEXT,                       -- For network: "B:Video/file.m2ts"
       share_name TEXT,                          -- Share name: "B", "G", etc.
       file_name TEXT NOT NULL,
       hash TEXT,
       links TEXT,
       is_network_file BOOLEAN DEFAULT 0,       -- True if on network mount
       created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
   );
   ```

### How It Works

#### 1. **Path Detection**

```cpp
auto& mount_manager = MountManager::getInstance();
bool is_network = mount_manager.isNetworkPath("/Volumes/truenas._smb._tcp.local-1/Video/file.m2ts");
// Returns: true
```

#### 2. **Path Conversion**

```cpp
// Absolute to Relative
auto relative = mount_manager.toRelativePath("/Volumes/truenas._smb._tcp.local-1/Video/file.m2ts");
// Returns: {share_name: "B", relative_path: "Video/file.m2ts", file_name: "file.m2ts"}

// Relative to Absolute
auto absolute = mount_manager.toAbsolutePath(*relative);
// Returns: "/Volumes/truenas._smb._tcp.local-1/Video/file.m2ts"
```

#### 3. **Database Storage**

- **Local files**: Store absolute path only
- **Network files**: Store both absolute and relative paths
- **Fallback**: If mount changes, use relative path to find new absolute path

### Benefits

1. **Mount Change Resilience**

   - Paths remain valid even when mount points change
   - Automatic resolution to new mount locations

2. **Backward Compatibility**

   - Existing absolute paths still work
   - Gradual migration to relative paths

3. **Transparent Operation**

   - No changes needed in processing logic
   - Automatic path resolution at runtime

4. **Cross-Platform Support**
   - Works on macOS, Linux, Windows
   - Handles SMB, NFS, AFP, CIFS

### Migration Strategy

1. **Database Migration**

   ```bash
   # Add new columns to existing database
   ./migrate_to_relative_paths
   ```

2. **Gradual Adoption**

   - New scans use relative paths for network files
   - Existing files work with both absolute and relative paths
   - Automatic fallback to absolute paths

3. **Validation**
   ```bash
   # Test mount detection and path conversion
   ./test_mount_manager
   ```

### Implementation Details

#### Mount Detection

- **macOS**: Uses `mount` command output
- **Linux**: Reads `/proc/mounts`
- **Cross-platform**: Detects SMB, NFS, AFP, CIFS mounts

#### Path Resolution

- **Longest match**: Finds the most specific mount point
- **Share matching**: Matches by share name (B, G, etc.)
- **Fallback**: Uses absolute path if relative resolution fails

#### Database Queries

```sql
-- Find files by relative path (preferred for network files)
SELECT * FROM scanned_files WHERE relative_path = 'B:Video/file.m2ts'

-- Fallback to absolute path
SELECT * FROM scanned_files WHERE file_path = '/Volumes/.../file.m2ts'

-- Combined query for network files
SELECT * FROM scanned_files WHERE relative_path = ? OR file_path = ?
```

### Configuration

The solution requires no configuration changes. It automatically:

1. **Detects network mounts** at runtime
2. **Converts paths** as needed
3. **Handles mount changes** transparently
4. **Maintains compatibility** with existing data

### Testing

```bash
# Test mount detection
./test_mount_manager

# Test database migration
./migrate_to_relative_paths

# Test file processing with new paths
curl -X POST http://localhost:8080/process/file \
  -H "Authorization: Bearer <token>" \
  -H "Content-Type: application/json" \
  -d '{"file_path": "/Volumes/truenas._smb._tcp.local-1/Video/file.m2ts"}'
```

### Future Enhancements

1. **Mount Monitoring**

   - Watch for mount/unmount events
   - Automatically refresh path cache
   - Notify processing system of changes

2. **Path Validation**

   - Periodic validation of stored paths
   - Automatic cleanup of invalid entries
   - Reporting of mount issues

3. **Performance Optimization**
   - Cache mount information
   - Batch path conversions
   - Lazy path resolution

This solution provides a robust, mount-aware approach to handling network file paths that will prevent the issues you experienced with changing SMB mount points.
