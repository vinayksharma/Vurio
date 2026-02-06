## Image Processing Architecture

This document describes the end-to-end image processing pipeline in the Media Dedup Server, covering control flow, threading, configuration, storage, and the three processing tiers (Fast, Balanced, Quality). It reflects the current implementation status in this repository.

### High-level Goals

- Reliable, asynchronous processing of images discovered by the file scanner
- SAFE TREO principles in the processing path: Timeouts, Retries, Error isolation, Observability
- All artifacts persisted in the database (no on-disk indexes)
- CPU-only stack; external libraries detected via CMake and optional at build time

### Key Components

- Files discovery: `FilesManager` emits candidate files into the database table `scanned_files`. Status codes per mode: -1 error, 0 ready, 1 picked up, 2 done.
- Orchestration: `MediaProcessor` receives unprocessed files by mode and routes to the correct media category (currently images). It uses `ThreadPoolManager` for fire-and-forget lambdas.
- Threading: `ThreadPoolManager` wraps Poco::ThreadPool with config-driven idle timeout and per-task shares. Lambdas submit work for `image_processor` task type.
- SAFE TREO wrapper: `ImageTask` encapsulates a single-file unit of work with timeouts, retries, and structured logging (observability). It executes a chosen pipeline and updates status in `scanned_files`.
- Pipelines: Three tiers
  - Fast: thumbnail + perceptual hash (OpenCV), saved as 64-bit pHash BLOB and thumb size
  - Balanced: local features (OpenCV ORB/SIFT) serialized and saved as BLOB; includes resize and keypoint capping
  - Quality: deep embeddings (CLIP image encoder via ONNX Runtime), saved as float32 BLOB with model metadata
- Storage: `image_artifacts` table holds per-image outputs across tiers.

### Data Model

- `scanned_files` (existing): tracks file metadata and per-mode processing flags; unchanged files are not reset erroneously thanks to strict metadata comparison (size, createdAt, modifiedAt).
- `image_artifacts` (new):
  - file_path (PK), phash BLOB, thumb_w, thumb_h
  - features_method TEXT, features BLOB
  - embedding_model TEXT, embedding_dim INTEGER, embedding BLOB
  - version, created_at, updated_at

### Control Flow

1. Server starts: `ServerInitializer` ensures crash-recovery by clearing only in-progress (1) flags back to 0 once.
2. Scheduler triggers `MediaProcessor::ProcessMedia` periodically (interval cfg). It queries `scanned_files` for ready files (status=0 for the active mode).
3. For each file, `MediaProcessor` submits a lambda to `ThreadPoolManager` with task name `image_processor`.
4. The lambda constructs `ImageTask` specifying TREO config (timeouts, retries), then invokes a tier-specific pipeline:
   - Fast → `FastPipeline::Run`
   - Balanced → `BalancedPipeline::Run`
   - Quality → `QualityPipeline::Run`
5. `ImageTask` updates `scanned_files` to -1 or 2 based on success/failure, with start/finish logging.

### SAFE TREO Details

- Timeouts: `ImageTaskConfig.timeout_ms` controls max per-file processing time. If exceeded, the task is treated as failed (retryable or not based on error type) and the DB is updated.
- Retries: `retry.enabled`, `maxAttempts`, `baseDelayMs` implement limited exponential backoff for transient issues (I/O, temporary ONNX/OpenCV errors). Non-retryable cases (decode/model errors) fail immediately.
- Error isolation: Tasks run as independent lambdas; failures are contained and logged with context. No unhandled exceptions escape to the pool.
- Observability: Structured logs at start/finish with duration, outcome, and artifact stats; warnings for unsupported types or missing models.

### Backends & Pipelines

- OpenCV Backend (`OpenCvAdapter`)

  - Disables internal threading to respect global thread pool concurrency
  - Loads image, resizes with aspect ratio, computes pHash via `cv::img_hash::pHash`
  - Derives a stable 64-bit representation for storage

- Features Backend (`FeaturesAdapter`)

  - Loads image, optional grayscale, resizes moderately
  - Extracts ORB/SIFT keypoints and descriptors; caps top N keypoints (config)
  - Serializes compact representation to BLOB

- ONNX Backend (`OnnxAdapter`)
  - Preprocess: sRGB conversion, resize to model input (e.g., 224×224), normalization
  - Loads an image-encoder ONNX model (CLIP ViT-B/32 or RN50 image encoder)
  - Runs inference (CPU), returns embedding vector (e.g., 512-D float32)
  - Handles missing/invalid model path by failing the task (no downgrade)

### Configuration

Relevant keys (subset):

- Thread pool:

  - `tpm.thread.idleTimeoutSeconds`: idle timeout; triggers pool recreation on change
  - `tpm.types.media_processor.share`: share for media processing tasks (consolidated naming)
  - `tpm.types.image_processor.share`: reserved for future dedicated image processor

- Scheduler:

  - `media.processor.intervalMs`: processing trigger interval

- Image Task (TREO):

  - `media.image.timeoutMs`
  - `media.image.retry.enabled`
  - `media.image.retry.maxAttempts`
  - `media.image.retry.baseDelayMs`

- Fast tier:

  - `media.image.fast.thumbSize`

- Balanced tier:

  - `media.image.balanced.resizeLongEdge`
  - `media.image.balanced.maxKeypoints`

- Quality tier:
  - `media.image.quality.onnx.modelPath`
  - `media.image.quality.onnx.inputSize`
  - `media.image.quality.embeddingDim`

All config lookups are performed via `UnifiedObservableConfigManager`; components subscribe to changes where applicable (e.g., thread pool idle timeout, shares).

### Error Handling & Status Codes

- Status codes in `scanned_files` per mode:
  - -1: permanent failure
  - 0: ready to process
  - 1: picked up (in queue)
  - 2: processed successfully
- Unsupported type or missing model: mark as -1 and log
- DB write errors (e.g., locked): retried by `ImageTask` based on retry policy; final failure sets -1.

### Build & Dependencies

- CMake detects OpenCV and ONNX Runtime; compilation guards:
  - `HAVE_OPENCV`, `HAVE_ONNXRUNTIME`
- ONNX headers are vendored automatically if not found (FetchContent)
- Example app `quality_smoke` validates Quality path end-to-end

### Extension Points

- New pipelines (e.g., denoise) can follow the `Run(file, cfg, db)` pattern and be wired in `MediaProcessor` routing.
- Additional artifact types can extend `image_artifacts` table (e.g., color histograms) with corresponding upsert ops.
- Future duplicate detection will consume artifacts to compute similarity (out of scope for current phase).

### Current Status

- Fast, Balanced, Quality scaffolds are implemented; Fast and Quality paths write artifacts to DB. Quality uses a known-good CLIP image encoder ONNX model.
- Unit tests cover DB ops and processor routing; `quality_smoke` example verifies an embedding row is created.
