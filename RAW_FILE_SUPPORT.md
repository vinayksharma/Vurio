# Raw File Support Implementation

## Overview

The dedup server now supports camera raw file formats with automatic transcoding functionality. Raw files are detected during scanning, queued for transcoding to JPEG format, and processed using the transcoded version.

## Supported Raw Formats

The system now supports the following camera raw file formats:

### Canon
- **CR2** - Canon Raw 2
- **CR3** - Canon Raw 3

### Nikon
- **NEF** - Nikon Electronic Format

### Sony
- **ARW** - Sony Alpha Raw

### Adobe
- **DNG** - Digital Negative

### Fujifilm
- **RAF** - Fujifilm Raw

### Olympus
- **ORF** - Olympus Raw Format

### Pentax
- **PEF** - Pentax Electronic Format

### Samsung
- **SRW** - Samsung Raw

### Kodak
- **KDC** - Kodak Digital Camera
- **DCR** - Kodak Digital Camera Raw

### Minolta
- **MOS** - Minolta Raw
- **MRW** - Minolta Raw

### Generic
- **RAW** - Generic Raw
- **BAY** - Bayer Pattern Raw

### Phase One
- **3FR** - Phase One Raw
- **FFF** - Phase One Raw

### Mamiya
- **MEF** - Mamiya Raw

### Hasselblad
- **IIQ** - Hasselblad Raw

### Ricoh
- **RWZ** - Ricoh Raw

### Nikon (Additional)
- **NRW** - Nikon Raw
- **RWL** - Nikon Raw

### Red Digital Cinema
- **R3D** - Red Raw

### Medical Imaging
- **DCM** - DICOM
- **DICOM** - Digital Imaging and Communications in Medicine

## Architecture

### 1. TranscodingManager

A new singleton class that manages raw file transcoding:

- **Independent Threads**: Uses separate transcoding threads to avoid blocking main scanning/processing
- **FFmpeg Integration**: Uses FFmpeg for transcoding raw files to JPEG format
- **Cache Management**: Stores transcoded files in a cache directory for reuse
- **Database Integration**: Tracks transcoding status in the `cache_map` table

### 2. File Scanner Integration

Modified `FileScanner` to detect raw files during scanning:

- Detects raw files using `TranscodingManager::isRawFile()`
- Queues raw files for transcoding using `TranscodingManager::queueForTranscoding()`
- Still stores raw files in `scanned_files` table for tracking

### 3. Processing Orchestrator Integration

Modified `MediaProcessingOrchestrator` to use transcoded files:

- Checks for transcoded version using `TranscodingManager::getTranscodedFilePath()`
- Uses transcoded file for processing instead of original raw file
- Falls back to original file if transcoding fails

### 4. Database Schema

Enhanced `cache_map` table usage:

```sql
CREATE TABLE cache_map (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    source_file_path TEXT NOT NULL UNIQUE,
    transcoded_file_path TEXT,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (source_file_path) REFERENCES scanned_files(file_path) ON DELETE CASCADE
);
```

## Workflow

### 1. Scanning Phase
```
Raw File Detected → Queue for Transcoding → Store in scanned_files
```

### 2. Transcoding Phase
```
Transcoding Thread → FFmpeg Processing → Store in Cache → Update Database
```

### 3. Processing Phase
```
Check for Transcoded File → Use Transcoded File → Process with MediaProcessor
```

## Configuration

### Cache Directory
- Default: `./cache`
- Configurable via `TranscodingManager::initialize()`

### Transcoding Threads
- Default: 4 threads (same as processing threads)
- Configurable via `TranscodingManager::initialize()`

### FFmpeg Settings
- Output format: JPEG
- Quality: `-q:v 2` (high quality)
- Command: `ffmpeg -i input.raw -y -q:v 2 output.jpg`

## File Naming Convention

Transcoded files use a hash-based naming scheme:
```
{hash}_{original_extension}.jpg
```

Example: `a1b2c3d4e5f6_cr2.jpg`

## Error Handling

- **Transcoding Failures**: Logged and skipped, original file remains in database
- **Missing FFmpeg**: Logged as error, transcoding skipped
- **File System Errors**: Graceful handling with appropriate logging
- **Database Errors**: Transaction rollback and error reporting

## Performance Considerations

### Benefits
- **Faster Processing**: JPEG files process much faster than raw files
- **Reduced Memory Usage**: JPEG files are significantly smaller
- **Better Compatibility**: JPEG format is universally supported
- **Caching**: Transcoded files are reused for subsequent processing

### Trade-offs
- **Storage Space**: Cache directory requires additional storage
- **Transcoding Time**: Initial transcoding adds processing time
- **Quality Loss**: JPEG compression may reduce image quality slightly

## Testing

### Test Program
Run the raw file test to verify functionality:
```bash
./build/raw_file_test
```

### Expected Output
```
Testing Raw File Support
=======================

Testing file extension detection:
test_image.cr2: Supported=Yes, Raw=Yes
test_image.nef: Supported=Yes, Raw=Yes
test_image.arw: Supported=Yes, Raw=Yes
...

Raw file extensions supported:
cr2: ✓
nef: ✓
arw: ✓
...
```

## Integration Points

### Main Application
- `main.cpp`: Initializes and starts TranscodingManager
- `file_scanner.cpp`: Detects and queues raw files
- `media_processing_orchestrator.cpp`: Uses transcoded files for processing

### Database Operations
- `database_manager.cpp`: Manages cache_map table operations
- `cache_map` table: Tracks transcoding status and file paths

### File System
- Cache directory: Stores transcoded JPEG files
- FFmpeg: External transcoding tool

## Future Enhancements

### Potential Improvements
1. **Quality Settings**: Configurable transcoding quality
2. **Format Options**: Support for other output formats (PNG, TIFF)
3. **Batch Processing**: Optimized batch transcoding
4. **Cache Cleanup**: Automatic cleanup of old transcoded files
5. **Progress Tracking**: Real-time transcoding progress reporting

### Additional Raw Formats
- Support for more camera-specific raw formats
- Better format detection and handling
- Camera-specific transcoding optimizations

## Dependencies

### Required
- **FFmpeg**: For raw file transcoding
- **OpenCV**: For image processing
- **SQLite**: For database operations
- **TBB**: For thread management

### Optional
- **libvips**: For enhanced image processing (BALANCED mode)
- **ONNX Runtime**: For CNN embeddings (QUALITY mode)

## Troubleshooting

### Common Issues

1. **FFmpeg Not Found**
   - Install FFmpeg: `brew install ffmpeg` (macOS)
   - Verify installation: `which ffmpeg`

2. **Transcoding Failures**
   - Check file permissions
   - Verify raw file format support
   - Check FFmpeg error logs

3. **Cache Directory Issues**
   - Ensure write permissions
   - Check available disk space
   - Verify directory creation

4. **Database Errors**
   - Check SQLite permissions
   - Verify database schema
   - Check for concurrent access issues

## API Integration

The raw file support is transparent to the API layer. Raw files are handled automatically during scanning and processing phases, with no changes required to the REST API interface. 