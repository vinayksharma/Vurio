# Media Processor Manual Fix Required

## Status

**CRITICAL**: media_processor.cpp has compilation errors due to mode-specific logic that must be removed.

## Problem

The automated sed script created syntax errors. Manual fixes are required.

## Required Manual Fixes

### 1. Remove `getCurrentServerMode()` method (Line ~1100)

**Find:**

```cpp
ServerMode MediaProcessor::getCurrentServerMode() const
{
    return config_manager_->getServerMode("server.mode", ServerMode::FAST);
}
```

**Delete entirely** - This method is no longer needed.

### 2. Update ProcessMedia() method (Lines ~800-850)

**Find:**

```cpp
ServerMode current_mode = getCurrentServerMode();
std::string mode_str;
// ... mode string conversion code ...
```

**Replace with:**

```cpp
// Single embedding-based processing mode
```

**Also find:**

```cpp
std::vector<ScannedFileRow> unprocessed_files = ScannedFilesOps::listUnprocessed(*database_manager_, current_mode, static_cast<int>(available_space));
```

**Replace with:**

```cpp
std::vector<ScannedFileRow> unprocessed_files = ScannedFilesOps::listUnprocessed(*database_manager_, static_cast<int>(available_space));
```

### 3. Remove all switch statements checking processed status

**Pattern to find (appears ~6 times):**

```cpp
int current_status = 0;
switch (server_mode_copy)
{
case ServerMode::FAST:
    current_status = current_file->processed_fast;
    break;
case ServerMode::BALANCED:
    current_status = current_file->processed_balanced;
    break;
case ServerMode::QUALITY:
    current_status = current_file->processed_quality;
    break;
}
```

**Replace with:**

```cpp
int current_status = current_file->processed;
```

### 4. Simplify processor dispatch calls

**Find repeated patterns like:**

```cpp
switch (server_mode_copy)
{
case ServerMode::FAST:
    processing_success = image_processor.ProcessFast(...);
    break;
case ServerMode::BALANCED:
    processing_success = image_processor.ProcessBalanced(...);
    break;
case ServerMode::QUALITY:
    processing_success = image_processor.ProcessQuality(...);
    break;
}
```

**Replace with:**

```cpp
bool processing_success = image_processor.Process(...);
```

**Do this for:**

- Image processor (line ~350)
- Video processor (line ~570)
- Audio processor (line ~760)

### 5. Remove server_mode_copy from lambda captures

**Find:**

```cpp
std::string file_path_copy = file_path;
ServerMode server_mode_copy = server_mode;
std::shared_ptr<DatabaseManager> db_manager = database_manager_;
```

**Replace with:**

```cpp
std::string file_path_copy = file_path;
std::shared_ptr<DatabaseManager> db_manager = database_manager_;
```

**And update lambda captures from:**

```cpp
[file_path_copy, server_mode_copy, db_manager, ...]
```

**To:**

```cpp
[file_path_copy, db_manager, ...]
```

### 6. Update ScannedFilesOps calls to remove mode parameter

**Replace:**

```cpp
ScannedFilesOps::markProcessed(*db_manager, file_path_copy, server_mode_copy, state);
ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, server_mode_copy, state);
ProcessingErrorsOps::insertError(*db_manager, file_path_copy, server_mode_copy, ...);
```

**With:**

```cpp
ScannedFilesOps::markProcessed(*db_manager, file_path_copy, state);
ScannedFilesOps::markProcessedWithEscalation(*db_manager, file_path_copy, state);
ProcessingErrorsOps::insertError(*db_manager, file_path_copy, ...);
```

## Testing

After fixes:

1. Run: `./build.sh 2>&1 | tee build.log`
2. Check for remaining compilation errors
3. Fix iteratively

## Estimated Time

2-3 hours of careful manual editing.
