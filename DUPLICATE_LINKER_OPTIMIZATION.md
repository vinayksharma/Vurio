# DuplicateLinker Optimization

## Overview

The DuplicateLinker has been optimized to avoid unnecessary duplicate detection runs when no relevant data has changed. This optimization uses hash-based change detection to determine whether duplicate detection is needed.

## Implementation Details

### 1. Database Schema Changes

**Modified `flags` table:**

```sql
CREATE TABLE IF NOT EXISTS flags (
    name TEXT PRIMARY KEY,
    value TEXT NOT NULL DEFAULT '0',  -- Changed from INTEGER to TEXT
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);
```

**Reason:** The `value` field now stores SHA256 hashes (64 characters) instead of just boolean values.

### 2. Hash Generation

**Combined Hash Method:** `SHA256(hash1|hash2|hash3)`

The system generates a combined hash from three relevant tables:

- `scanned_files` - Files discovered during scanning
- `cache_map` - Cache mapping for transcoded files
- `media_processing_results` - Results from media processing

**Process:**

1. Generate individual SHA256 hash for each table
2. Concatenate with `|` delimiter: `hash1|hash2|hash3`
3. Hash the concatenated string: `SHA256(hash1|hash2|hash3)`

### 3. Optimization Logic

**Before each duplicate detection run:**

1. **Generate current hash** of the three relevant tables
2. **Retrieve stored hash** from `flags` table with `name = 'dedup_linker_state'`
3. **Compare hashes:**
   - **Match**: Skip duplicate detection, log info message
   - **Different**: Run duplicate detection, update stored hash
   - **No stored hash**: Run duplicate detection (first run), store hash

**Hash Storage Timing:**

- Hash is stored **after** duplicate detection completes successfully
- This ensures that if the server crashes during duplicate detection, the next run will have the opportunity to complete the task
- Only stores hash when duplicate detection actually runs and completes

### 4. Error Handling

**Graceful Fallback:**

- If hash generation fails → Run duplicate detection anyway
- If hash storage fails → Continue with duplicate detection
- If flag retrieval fails → Run duplicate detection

**Logging:**

- **INFO level**: "skipping duplicate detection - no relevant data changes detected"
- **DEBUG level**: "will run duplicate detection - data changes detected or first run"
- **WARN level**: Hash generation/storage failures

### 5. Full Rescan Override

**Full rescan requests bypass optimization:**

- When `needs_full_rescan_` is true
- When periodic full rescan is triggered
- Always runs duplicate detection regardless of hash state

## API Methods

### DatabaseManager

```cpp
// Generate combined hash of relevant tables
std::pair<bool, std::string> getDuplicateDetectionHash();

// Get TEXT flag value
std::string getTextFlag(const std::string &flag_name);

// Set TEXT flag value
DBOpResult setTextFlag(const std::string &flag_name, const std::string &value);
```

## Behavior Examples

### Scenario 1: No Data Changes

```
1. DuplicateLinker starts
2. Generate current hash: "abc123..."
3. Retrieve stored hash: "abc123..."
4. Hashes match → Skip duplicate detection
5. Log: "skipping duplicate detection - no relevant data changes detected"
6. Wait for next scheduled run
```

### Scenario 2: Data Changes

```
1. DuplicateLinker starts
2. Generate current hash: "def456..."
3. Retrieve stored hash: "abc123..."
4. Hashes differ → Run duplicate detection
5. Process duplicates normally
6. Store new hash: "def456..." (after completion)
7. Log: "will run duplicate detection - data changes detected"
```

### Scenario 3: First Run

```
1. DuplicateLinker starts
2. Generate current hash: "abc123..."
3. Retrieve stored hash: "" (empty)
4. No stored hash → Run duplicate detection
5. Process duplicates normally
6. Store hash: "abc123..." (after completion)
7. Log: "will run duplicate detection - data changes detected or first run"
```

## Performance Benefits

### Before Optimization:

- Duplicate detection runs every 10 seconds (configurable)
- Always processes all files regardless of changes
- CPU and I/O intensive operations run unnecessarily

### After Optimization:

- Duplicate detection only runs when relevant data changes
- Significant reduction in CPU usage during idle periods
- Faster response times for other operations
- Reduced database load

## Configuration

**No new configuration required:**

- Uses existing `duplicate_linker_check_interval` setting
- Optimization is always enabled
- No way to disable (as per requirements)

## Testing

**Test Script:** `tests/scripts/test_duplicate_linker_optimization.sh`

**Tests:**

1. Initial hash generation and storage
2. Skipping duplicate detection when no changes
3. Running duplicate detection when data changes
4. Error handling and fallback behavior

## Monitoring

**Key Log Messages:**

- `"skipping duplicate detection - no relevant data changes detected"`
- `"will run duplicate detection - data changes detected or first run"`
- `"Failed to generate duplicate detection hash, will run duplicate detection anyway"`

**Hash Storage:**

- Flag name: `dedup_linker_state`
- Value: SHA256 hash of combined table data
- Updated: Before each duplicate detection run

## Technical Notes

### Hash Characteristics

- **Deterministic**: Same data always produces same hash
- **Content-sensitive**: Any change in relevant tables changes hash
- **Order-dependent**: Uses `ORDER BY rowid` for consistent ordering
- **SHA256**: Industry-standard cryptographic hashing

### Performance Impact

- **Hash generation**: Minimal overhead (reads table data once)
- **Hash comparison**: Instant string comparison
- **Storage**: Single database write per run
- **Memory**: Temporary string storage during hash generation

### Reliability

- **Graceful degradation**: Falls back to full duplicate detection on errors
- **No data loss**: Optimization only affects when duplicate detection runs, not how it runs
- **Consistent behavior**: Full rescan requests always bypass optimization
- **Crash recovery**: If server crashes during duplicate detection, next run will complete the task
