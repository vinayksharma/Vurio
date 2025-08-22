# Dedup Server Kill Scripts

This directory contains scripts to safely terminate running instances of the dedup-server.

## Scripts Overview

### 1. `kill_dedup_server.sh` - Main Kill Script

The primary script for terminating dedup-server instances with graceful shutdown support.

### 2. `kill_server.sh` - Quick Alias

A simple wrapper script that calls the main kill script.

## Usage

### Basic Usage

```bash
# Kill all dedup-server instances gracefully
./kill_dedup_server.sh

# Quick alias
./kill_server.sh
```

### Advanced Options

```bash
# Force kill with SIGKILL if SIGTERM fails
./kill_dedup_server.sh --force

# Show detailed process information
./kill_dedup_server.sh --verbose

# Combine options
./kill_dedup_server.sh --force --verbose

# Show help
./kill_dedup_server.sh --help
```

## How It Works

### 1. Process Discovery

- Searches for all processes containing "dedup_server" in their command line
- Uses `pgrep -f "dedup_server"` for comprehensive process detection

### 2. Graceful Shutdown

- **First attempt**: Sends `SIGTERM` to all discovered processes
- **Wait period**: Allows 3 seconds for graceful shutdown
- **Verification**: Checks which processes are still running

### 3. Force Termination (if enabled)

- **Second attempt**: Sends `SIGKILL` to stubborn processes
- **Final wait**: Additional 1 second for cleanup
- **Status check**: Verifies all processes have terminated

### 4. Cleanup

- **Stale PID file detection**: Automatically identifies and removes PID files containing non-running PIDs
- **Process verification**: Checks if PIDs in files are actually running before cleanup
- **Automatic cleanup**: Removes stale `dedup_server.pid` files to prevent startup conflicts
- Reports final status

## Exit Codes

- **0**: All instances successfully terminated
- **1**: Some instances still running (or error occurred)

## Safety Features

✅ **Graceful shutdown first** - Always tries SIGTERM before SIGKILL  
✅ **Process verification** - Checks if processes actually exist before signaling  
✅ **Error handling** - Gracefully handles missing or terminated processes  
✅ **PID file cleanup** - Removes stale PID files automatically  
✅ **Colored output** - Easy-to-read status messages  
✅ **Verbose mode** - Detailed process information when needed

## Examples

### Scenario 1: Normal Shutdown

```bash
$ ./kill_dedup_server.sh
🔍 Searching for running dedup-server instances...
Found 2 dedup-server instance(s):
  PID: 12345
  PID: 12346
🛑 Attempting graceful shutdown...
Sending SIGTERM to PID 12345... ✓
Sending SIGTERM to PID 12346... ✓
⏳ Waiting for graceful shutdown (3 seconds)...
✅ All dedup-server instances successfully terminated
🧹 Cleaning up PID file...
✅ PID file removed
```

### Scenario 2: Force Kill Required

```bash
$ ./kill_dedup_server.sh --force
🔍 Searching for running dedup-server instances...
Found 1 dedup-server instance(s):
  PID: 12345
🛑 Attempting graceful shutdown...
Sending SIGTERM to PID 12345... ✓
⏳ Waiting for graceful shutdown (3 seconds)...
⚠️  Some processes still running, force killing with SIGKILL...
Sending SIGKILL to PID 12345... ✓
✅ All dedup-server instances successfully terminated
```

### Scenario 3: No Instances Running

```bash
$ ./kill_dedup_server.sh
🔍 Searching for running dedup-server instances...
✅ No dedup-server instances found running
```

## Troubleshooting

### Stale PID File Issue

**Problem**: After Ctrl+C, `run.sh` says "Another instance is already running" even though no processes are running.

**Cause**: Stale PID file containing a PID that's no longer active.

**Solution**: The kill script now automatically detects and removes stale PID files:

```bash
./kill_dedup_server.sh
```

**Manual fix** (if needed):

```bash
# Check if PID file exists
ls -la dedup_server.pid

# Check if PID in file is running
cat dedup_server.pid | xargs kill -0 2>/dev/null && echo "Running" || echo "Not running"

# Remove stale PID file
rm dedup_server.pid
```

### Process Still Running After Kill

If processes persist after running the script:

1. **Check for zombie processes**:

   ```bash
   ps aux | grep dedup_server | grep -v grep
   ```

2. **Use force mode**:

   ```bash
   ./kill_dedup_server.sh --force
   ```

3. **Manual verification**:
   ```bash
   pgrep -f "dedup_server"
   ```

### Permission Issues

If you get permission errors:

1. **Ensure script is executable**:

   ```bash
   chmod +x kill_dedup_server.sh
   ```

2. **Check file ownership**:
   ```bash
   ls -la kill_dedup_server.sh
   ```

## Integration

### With Development Workflow

```bash
# Quick restart during development
./kill_dedup_server.sh && ./run.sh

# Force cleanup before rebuild
./kill_dedup_server.sh --force && ./rebuild.sh
```

### With CI/CD

```bash
# Ensure clean state before deployment
./kill_dedup_server.sh --force --verbose
```

## Notes

- The script uses `pgrep -f` which searches the full command line, ensuring it catches all dedup-server processes
- SIGTERM allows the server to perform graceful shutdown and cleanup
- SIGKILL is only used when explicitly requested with `--force`
- The script automatically cleans up PID files to prevent startup conflicts
- All operations are logged with clear, colored status messages

## Recent Improvements

### ✅ **Ctrl+C Now Automatically Cleans Up PID Files**

**Before**: Ctrl+C would leave stale PID files, requiring manual cleanup or using the kill script.

**After**: Ctrl+C now automatically removes the PID file, preventing startup conflicts.

**How it works**:

1. **Signal handler intercepts** SIGINT (Ctrl+C), SIGTERM, and SIGQUIT
2. **Immediately cleans up** the PID file to prevent stale file issues
3. **Gracefully exits** with proper cleanup
4. **No more manual intervention** needed after Ctrl+C

**Result**: You can now Ctrl+C and immediately run `run.sh` without any conflicts! 🎉
