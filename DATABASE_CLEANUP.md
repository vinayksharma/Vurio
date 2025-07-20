# Database Cleanup Scripts

This directory contains scripts to clear out the dedup server database and related files.

## Scripts Available

### 1. `clear_database.sh` - Basic Cleanup Script

A simple script that removes all database files and related temporary files.

**Usage:**

```bash
./clear_database.sh
```

**What it removes:**

- Main database files (`scan_results.db*`)
- Test database files (`test_*.db*`)
- Test directories (`test_orchestrator_files`, `test_*`)
- Temporary files (`config.json`, `*.tmp`, `*.log`)
- Build artifacts containing database references

### 2. `clear_database_advanced.sh` - Advanced Cleanup Script

A comprehensive script with safety features and additional options.

**Usage:**

```bash
./clear_database_advanced.sh [OPTIONS]
```

**Options:**

- `--help, -h` - Show help message
- `--backup, -b` - Create backup before cleanup
- `--dry-run, -d` - Show what would be deleted without actually deleting
- `--force, -f` - Skip confirmation prompts
- `--verbose, -v` - Show detailed output

**Examples:**

```bash
# Normal cleanup with confirmation
./clear_database_advanced.sh

# Backup then cleanup
./clear_database_advanced.sh --backup

# Show what would be deleted (dry run)
./clear_database_advanced.sh --dry-run

# Skip confirmation prompts
./clear_database_advanced.sh --force

# Create backup and skip confirmation
./clear_database_advanced.sh --backup --force
```

## What Gets Cleaned

### Database Files

- `scan_results.db` - Main database file
- `scan_results.db-wal` - Write-Ahead Log file
- `scan_results.db-shm` - Shared Memory file
- `test_orchestrator.db*` - Test database files
- Any other `*.db*` files

### Test Files

- `test_orchestrator_files/` - Test directory
- Any `test_*` directories
- Test database files in build directory

### Temporary Files

- `config.json` - Configuration file
- `*.tmp` - Temporary files
- `*.log` - Log files

### Build Artifacts

- Database files in `build/` directory
- Test directories in `build/` directory

## Safety Features

### Backup Functionality

The advanced script can create a timestamped backup before cleanup:

```bash
./clear_database_advanced.sh --backup
```

This creates a directory like `database_backup_20241220_143022/` containing all database files.

### Confirmation Prompts

By default, the advanced script asks for confirmation before deleting files:

```
⚠️  This will permanently delete all database files and related data.
Are you sure you want to continue? (y/N)
```

### Dry Run Mode

Test what would be deleted without actually deleting anything:

```bash
./clear_database_advanced.sh --dry-run
```

## When to Use

### Use Basic Script When:

- You want a quick cleanup
- You're in a development environment
- You don't need safety features

### Use Advanced Script When:

- You want to backup data first
- You're in a production environment
- You want to see what will be deleted first
- You want colored output and detailed feedback

## After Cleanup

Once the database is cleared:

1. The dedup server will start with a fresh database
2. All scanned files and processing results will be lost
3. You'll need to re-scan and re-process any files
4. Configuration will be reset to defaults

## Recovery

If you used the `--backup` option, you can restore from the backup:

```bash
# Find the backup directory
ls -la database_backup_*

# Copy files back (example)
cp database_backup_20241220_143022/*.db* ./
```

## Notes

- Both scripts are safe to run multiple times
- Scripts handle missing files gracefully
- Build artifacts are cleaned to prevent test contamination
- The advanced script provides colored output for better visibility
