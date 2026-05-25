# Ejection Folder Migration

The user wants to migrate all legacy `UUID_ejected` folders to the new `ejected_UUID` format using a migration script, and then completely remove the backward compatibility support from `file_scanner.cpp`.

## Plan
1. **Create Python Migration Script**: 
   - Connect to `data/dedup_server.db`.
   - Find all `ejectionPath`, `ejectionPathErrored`, and `ejectionPathIgnored` entries in `user_settings` that end with `_ejected`, `_errored`, or `_ignored`.
   - For each legacy folder:
     - Determine the new path (e.g., `UUID_ejected` -> `ejected_UUID`).
     - Rename the physical directory on disk if it exists.
     - Update the `user_settings` table to point to the new path.
     - Execute `REPLACE` queries across all relevant database tables to update paths for files inside these folders:
       - `scanned_files` (`file_path`, `relative_path`)
       - `ejected_files_history` (`ejected_path`)
       - `duplicate_members` (`file_path`)
       - `duplicate_groups` (`representative_file_path`)
       - `image_artifacts` (`file_path`)
       - `processing_errors` (`file_path`)
       - `thumbnail_cache` (`source_path`)
       - `preview_cache` (`source_path`)
2. **Execute Migration Script**:
   - Run the script and verify that the database and filesystem are successfully migrated.
3. **Revert Scanner Fallback**:
   - Revert the changes made to `src/filesmanager/file_scanner.cpp` so it only ignores prefixes (`ejected_`, etc.) and no longer checks for suffixes.
