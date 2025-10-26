# Trace Logging Added

## Purpose

Added extensive trace-level logging to debug why ONNX computation is failing for all files.

## Changes Made

### 1. MediaProcessor (`src/media_processors/media_processor.cpp`)

Added trace logging to track the processing flow:

- Logs when about to process a file (with both processing_path and original_path)
- Logs before calling `image_processor.Process`
- Logs the result of `image_processor.Process` (true/false)

This helps identify:

- What file paths are being passed to the image processor
- Whether the processing succeeds or fails

### 2. OnnxAdapter (`src/media_processors/image/backends/onnx_adapter.cpp`)

Added comprehensive trace logging for ONNX image loading:

- Logs when `ComputeEmbedding` is called with all parameters (file_path, model_path, input_size, embedding_dim)
- Logs before attempting to load the image with OpenCV
- Logs the result of `cv::imread` (rows, cols, whether empty)
- Logs file existence check if the image is empty

This helps identify:

- What file path is being passed to ONNX computation
- Whether OpenCV can successfully load the image
- If the file exists but OpenCV returns empty
- What the image dimensions are

## How to View Trace Logs

The server is configured with `logging.level: trace` in `config/config.yaml`. To see the trace logs:

1. Check the console where the server is running
2. The logs will show detailed information about:
   - File paths being processed
   - Whether transcoding is producing valid files
   - Whether OpenCV can load the transcoded files
   - The exact point of failure

## Expected Debugging Flow

With these logs, you should be able to see:

1. **Transcoding stage**: Whether RAW files are being transcoded successfully
2. **File paths**: What path is being passed to the ONNX adapter
3. **OpenCV loading**: Whether `cv::imread` succeeds or returns empty
4. **File existence**: Whether the file exists when OpenCV tries to load it

## Current Status

- Errors are still accumulating (1,272+ errors)
- 0 files successfully processed
- ONNX errors dominate (680 out of latest 753 errors)
- All attempted files are CR2 RAW files

## Next Steps

1. Check the console output for trace-level logs
2. Look for the pattern:
   - `About to process file - processing_path: ...`
   - `ComputeEmbedding called - file_path: ...`
   - `cv::imread returned - rows: X, cols: Y, empty: true/false`
3. This will reveal whether:
   - Transcoding is producing invalid files
   - The file paths are incorrect
   - OpenCV cannot read the transcoded files

## Files Modified

- `src/media_processors/media_processor.cpp` - Added trace logging
- `src/media_processors/image/backends/onnx_adapter.cpp` - Added trace logging and filesystem include
- Server rebuilt and running
