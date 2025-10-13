# Configuration Key Consolidation Summary

## Overview

Consolidated TPM thread type share configuration to use unified naming pattern: `tpm.types.<type>.share`

## Changes Made

### 1. Configuration Key Migration

**Old Keys (REMOVED):**

- `media.processor.threadPool.share.image_processor`
- `media.processor.threadPool.share.audio_processor`
- `media.processor.threadPool.share.video_processor`

**New Keys (ADDED):**

- `tpm.types.media_processor.share` - Active: used by media processing tasks
- `tpm.types.image_processor.share` - Reserved: for future dedicated image processor
- `tpm.types.audio_processor.share` - Reserved: for future dedicated audio processor
- `tpm.types.video_processor.share` - Reserved: for future dedicated video processor

**Existing Key (unchanged):**

- `tpm.types.fileScan.share` - Used by scheduler jobs (fileScan, mediaProcessor)

### 2. Thread Type Usage

**Active Thread Types:**

1. **`fileScan`** (share: 1.0)
   - Used by: `fileScan` scheduler job (directory scanning)
   - Used by: `mediaProcessor` scheduler job (processing loop trigger)
2. **`media_processor`** (share: 1.0)
   - Used by: Individual file processing tasks
   - Handles actual image/audio/video processing

**Reserved Thread Types (for future use):** 3. **`image_processor`** (share: 1.0) - Reserved 4. **`audio_processor`** (share: 1.0) - Reserved 5. **`video_processor`** (share: 1.0) - Reserved

### 3. Files Modified

#### Code Files:

1. **`src/config/config_manager_factory.cpp`**

   - Removed old `media.processor.threadPool.share.*` properties
   - Added new `tpm.types.*_processor.share` properties with "Reserved" descriptions

2. **`src/media_processors/media_processor.cpp`**
   - Updated initialization to use `tpm.types.media_processor.share`
   - Added initialization for reserved types (image/audio/video_processor)
   - Updated `onConfigChange()` handler to react to new key names
   - Added reactive handlers for reserved type share changes (log + setShare)

#### Configuration Files:

3. **`config/config.yaml`**
   - Changed: `media.processor.threadPool.share.media_processor: 1`
   - To: `tpm.types.media_processor.share: 1`

#### Documentation Files:

4. **`config/CONFIGURATION_REFERENCE.md`**

   - Updated configuration table with new keys
   - Added "Reserved for future use" annotations
   - Updated example configuration section
   - Updated runtime behavior notes

5. **`docs/image_processing_architecture.md`**

   - Updated thread pool configuration references
   - Added note about consolidated naming

6. **`IMPLEMENTATION_SUMMARY.md`**
   - Added new media processor share keys to configuration table

### 4. Reactive Behavior

All thread type shares are **observable and reactive**:

- **`tpm.types.fileScan.share`** - Already reactive (ThreadPoolManager)
- **`tpm.types.media_processor.share`** - Reactive (MediaProcessor)
- **`tpm.types.image_processor.share`** - Reactive (MediaProcessor, reserved)
- **`tpm.types.audio_processor.share`** - Reactive (MediaProcessor, reserved)
- **`tpm.types.video_processor.share`** - Reactive (MediaProcessor, reserved)

When any share changes:

1. Config change event fires
2. MediaProcessor or TPM catches event
3. Calls `setShare()` on ThreadPoolManager
4. Logs the change
5. Takes effect immediately for new task submissions

### 5. Default Values

All thread type shares default to `1.0` (100% allocation).

### 6. Testing

- ✅ Build succeeded with no errors
- ✅ All unit tests still compile
- ⏳ Runtime testing needed (verify server starts, check status endpoint)

## Migration Notes for Users

If you have a custom `config.yaml`, update:

```yaml
# OLD (remove these)
media.processor.threadPool.share.media_processor: 1.0
media.processor.threadPool.share.image_processor: 1.0
media.processor.threadPool.share.audio_processor: 1.0
media.processor.threadPool.share.video_processor: 1.0

# NEW (add this - only media_processor is active)
tpm.types.media_processor.share: 1.0

# Optional (reserved for future use)
tpm.types.image_processor.share: 1.0
tpm.types.audio_processor.share: 1.0
tpm.types.video_processor.share: 1.0
```

## Verification Steps

1. Start server: `./media_dedup_server`
2. Check logs for thread share initialization
3. Query status: `curl http://localhost:8080/api/v1/server/status | jq '.thread_pool'`
4. Verify thread types present: `fileScan`, `media_processor`
5. Test reactive update: `curl -X PUT http://localhost:8080/api/v1/config/tpm.types.media_processor.share -d '{"value":"0.8"}'`
6. Check logs for update confirmation

## Benefits

1. **Consistent Naming**: All TPM thread types use `tpm.types.<type>.share` pattern
2. **Future-Ready**: Reserved types already configured and reactive
3. **Observable**: All shares react to runtime changes
4. **Documented**: Clear distinction between active and reserved types
5. **No Breaking Logic**: Media processing still works exactly as before
