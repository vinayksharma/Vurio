# Configuration Reference

This project uses a YAML configuration file that is auto-loaded and monitored at runtime. Values changed on disk are picked up automatically.

- File path: `config/config.yaml` (fallback: `./config.yaml`)
- Auto-reload interval: 1s
- Existing files are not overwritten at startup

## Properties

| Key                                        | Type    | Allowed values                                                                          | Default                      | Live effect                                         | Description                                                                                                                                                                 |
| ------------------------------------------ | ------- | --------------------------------------------------------------------------------------- | ---------------------------- | --------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `server.host`                              | string  | Any valid hostname/IP. `0.0.0.0` binds all                                              | `0.0.0.0`                    | Restart web server on change                        | Address to bind the HTTP server                                                                                                                                             |
| `server.port`                              | integer | 1–65535 (must be non-zero)                                                              | `8080`                       | Restart web server on change                        | TCP port for the HTTP server                                                                                                                                                |
| `server.name`                              | string  | Any non-empty string                                                                    | `Media Deduplication Server` | None (log-only)                                     | Friendly name shown in logs/status                                                                                                                                          |
| `server.mode`                              | string  | `FAST` \| `BALANCED` \| `QUALITY`                                                       | `FAST`                       | Applied live; drives processing/de-dup strategy     | Runtime processing profile                                                                                                                                                  |
| `server.processName`                       | string  | Process name for instance checking                                                      | `media_dedup_server`         | Applied live                                        | Executable name used by instance checker                                                                                                                                    |
| `server.instanceCheck.enabled`             | boolean | `true` \| `false`                                                                       | `true`                       | Applied live                                        | Enable single-instance guard                                                                                                                                                |
| `server.instanceCheck.bufferSize`          | integer | > 0                                                                                     | `128`                        | Applied live                                        | Internal buffer size for instance checking                                                                                                                                  |
| `database.path`                            | string  | File path; parent dirs will be auto-created                                             | `data/dedup_server.db`       | New path is honored on next DB (re)initialization   | SQLite database file path                                                                                                                                                   |
| `database.session.acquireTimeoutMs`        | integer | > 0                                                                                     | `3000`                       | Applied live (DB session acquire timeout)           | Max wait to acquire a DB session (ms)                                                                                                                                       |
| `database.session.acquireBackoffMs`        | integer | > 0                                                                                     | `50`                         | Applied live (retry backoff while waiting)          | Sleep between retries while acquiring a session (ms)                                                                                                                        |
| `database.session.poolMin`                 | integer | 1-50                                                                                    | `4`                          | Requires restart (pool size is set at init)         | Minimum database connection pool size                                                                                                                                       |
| `database.session.poolMax`                 | integer | 1-100 (must be >= poolMin)                                                              | `20`                         | Requires restart (pool size is set at init)         | Maximum database connection pool size                                                                                                                                       |
| `cache.disk.location`                      | string  | File path (relative to working directory root)                                          | `cache`                      | Applied live (relocates cache, recalculates size)   | Disk cache directory location                                                                                                                                               |
| `cache.disk.size_limit_mb`                 | integer | > 0                                                                                     | `2048`                       | Applied live (enforces new limit, evicts old files) | Maximum cache size in megabytes                                                                                                                                             |
| `cache.disk.clearOnStartup`                | boolean | `true` \| `false`                                                                       | `true`                       | Applied at initialization                           | Clear transcoding cache on startup (temporary files)                                                                                                                        |
| `cache.thumbnail.location`                 | string  | File path (relative to working directory root)                                          | `cache/thumbnails`           | Applied live (relocates cache, recalculates size)   | Thumbnail cache directory location                                                                                                                                          |
| `cache.thumbnail.size_limit_mb`            | integer | > 0                                                                                     | `512`                        | Applied live (enforces new limit, evicts old files) | Maximum thumbnail cache size in megabytes                                                                                                                                   |
| `cache.thumbnail.clearOnStartup`           | boolean | `true` \| `false`                                                                       | `false`                      | Applied at initialization                           | Clear thumbnail cache on startup (set false to persist)                                                                                                                     |
| `thumbnail.pipeline.enabled`               | boolean | `true` \| `false`                                                                       | `true`                       | Applied live                                        | Auto-generate thumbnails at end of processing pipeline                                                                                                                      |
| `thumbnail.pipeline.size`                  | integer | `128`, `256`, `512`, `1024`                                                             | `256`                        | Applied live                                        | Default thumbnail size for pipeline generation                                                                                                                              |
| `thumbnail.pipeline.onlyIfMissing`         | boolean | `true` \| `false`                                                                       | `true`                       | Applied live                                        | Skip generation if valid thumbnail already exists                                                                                                                           |
| `thumbnail.jpeg.quality`                   | integer | 0-100                                                                                   | `85`                         | Applied live                                        | JPEG quality for thumbnail generation (higher = better)                                                                                                                     |
| `media.adjustments.enabled`                | boolean | `true` \| `false`                                                                       | `true`                       | Applied live                                        | ADJ-1: non-destructive crop/rotate edits; off hides the panel and renders every image un-edited                                                                             |
| `media.adjustments.sidecar.write`          | boolean | `true` \| `false`                                                                       | `true`                       | Applied live                                        | ADJ-1: write edit sidecars beside the assets (or in the edits folder); off keeps edits in the database only                                                                 |
| `media.adjustments.sidecar.suffix`         | string  | non-empty suffix                                                                        | `.vurio.json`                | Applied live (new writes)                           | ADJ-1: sidecar name = asset file name + suffix; existing sidecars are not renamed                                                                                           |
| `thumbnail.generation.timeoutMs`           | integer | > 0                                                                                     | `5000`                       | Applied live                                        | Timeout for individual thumbnail generation (ms)                                                                                                                            |
| `logging.level`                            | string  | Case-insensitive: `trace`, `debug`, `info` (`information`), `warn` (`warning`), `error` | `info`                       | Applied live                                        | Global logging level (accepts synonyms in parentheses)                                                                                                                      |
| `files.manager.enabled`                    | boolean | `true` \| `false`                                                                       | `true`                       | Applied live                                        | Enable FilesManager scanning                                                                                                                                                |
| `files.manager.scan.intervalMs`            | integer | > 0                                                                                     | `500`                        | Applied live                                        | Interval between file scan cycles (ms)                                                                                                                                      |
| `scheduler.jitter.enabled`                 | boolean | `true` \| `false`                                                                       | `false`                      | Applied live                                        | Randomize schedule times to reduce coordinated spikes                                                                                                                       |
| `scheduler.jitter.percent`                 | integer | 0-100                                                                                   | `0`                          | Applied live                                        | Jitter percentage applied to schedules                                                                                                                                      |
| `scheduler.drift.mode`                     | string  | `anchored` \| `floating`                                                                | `anchored`                   | Applied live                                        | Scheduling anchor vs. drift-forward mode                                                                                                                                    |
| `scheduler.drift.maxDriftMs`               | integer | > 0                                                                                     | `60000`                      | Applied live                                        | Max allowed drift before correction (ms)                                                                                                                                    |
| `scheduler.backoff.enabled`                | boolean | `true` \| `false`                                                                       | `true`                       | Applied live                                        | Enable exponential backoff after failures                                                                                                                                   |
| `scheduler.backoff.initialMs`              | integer | > 0                                                                                     | `1000`                       | Applied live                                        | Initial backoff delay (ms)                                                                                                                                                  |
| `scheduler.backoff.multiplier`             | number  | > 1.0                                                                                   | `2.0`                        | Applied live                                        | Multiplier for each backoff step                                                                                                                                            |
| `scheduler.backoff.maxMs`                  | integer | > 0                                                                                     | `30000`                      | Applied live                                        | Upper bound for backoff delay (ms)                                                                                                                                          |
| `scheduler.backoff.jitterPercent`          | integer | 0-100                                                                                   | `10`                         | Applied live                                        | Randomization applied to backoff delays                                                                                                                                     |
| `tpm.pool.max`                             | string  | `auto` or integer                                                                       | `auto`                       | Applied live; decrease is gradual                   | Max TPM worker threads (`auto` = 75% of CPU cores + 1)                                                                                                                      |
| `tpm.killTimeoutMs`                        | integer | > 0                                                                                     | `10000`                      | Drain timeout on shutdown                           | Graceful shutdown timeout for in-flight tasks (ms)                                                                                                                          |
| `tpm.thread.idleTimeoutSeconds`            | integer | > 0                                                                                     | `120`                        | Applied live; recreates thread pool                 | Idle time before pool threads are reaped (s)                                                                                                                                |
| `tpm.types.<name>.share`                   | number  | (0,1]                                                                                   | `1.0`                        | Per-type slice; honor-based                         | Concurrency share for a TPM task type                                                                                                                                       |
| `server.max_connections`                   | integer | > 0                                                                                     | `100`                        | Applied live                                        | Max simultaneous HTTP connections                                                                                                                                           |
| `server.timeout`                           | number  | > 0.0                                                                                   | `30.0`                       | Applied live                                        | HTTP request timeout (s)                                                                                                                                                    |
| `server.http.threadPool.maxThreads`        | string  | `auto` or integer                                                                       | `auto`                       | Requires server restart                             | HTTP server thread pool size (`auto` = 75% of CPU cores + 1)                                                                                                                |
| `server.http.threadPool.maxQueued`         | integer | > 0                                                                                     | `50`                         | Requires server restart                             | HTTP request queue depth                                                                                                                                                    |
| `logging.enable_console`                   | boolean | `true` \| `false`                                                                       | `true`                       | Applied live                                        | Enable console logging output                                                                                                                                               |
| `logging.enable_file`                      | boolean | `true` \| `false`                                                                       | `false`                      | Applied live                                        | Enable log file output                                                                                                                                                      |
| `debug.enabled`                            | boolean | `true` \| `false`                                                                       | `false`                      | Applied live                                        | Enable extra diagnostics for development                                                                                                                                    |
| `debug.verbose`                            | boolean | `true` \| `false`                                                                       | `false`                      | Applied live                                        | Turn on very chatty debug logging                                                                                                                                           |
| `file_monitoring.enabled`                  | boolean | `true` \| `false`                                                                       | `true`                       | Applied live                                        | Enable config file change monitoring                                                                                                                                        |
| `file_monitoring.interval`                 | integer | > 0                                                                                     | `500`                        | Applied live                                        | Poll interval for config file monitor (ms)                                                                                                                                  |
| `validation.enabled`                       | boolean | `true` \| `false`                                                                       | `true`                       | Applied live                                        | Enable configuration validation                                                                                                                                             |
| `validation.strict`                        | boolean | `true` \| `false`                                                                       | `false`                      | Applied live                                        | Treat warnings as errors during validation                                                                                                                                  |
| `media.images.*`                           | boolean | `true` \| `false`                                                                       | `true`                       | Applied live (controls image format processing)     | Toggle processing for standard image formats                                                                                                                                |
| `media.images.raw.*`                       | boolean | `true` \| `false`                                                                       | `true`                       | Applied live (controls raw image format processing) | Toggle processing for raw image formats                                                                                                                                     |
| `media.video.*`                            | boolean | `true` \| `false`                                                                       | `true`                       | Applied live (controls video format processing)     | Toggle processing for video formats                                                                                                                                         |
| `media.audio.*`                            | boolean | `true` \| `false`                                                                       | `true`                       | Applied live (controls audio format processing)     | Toggle processing for audio formats                                                                                                                                         |
| `media.processor.enabled`                  | boolean | `true` \| `false`                                                                       | `true`                       | Applied live (enables/disables media processing)    | Master switch for media processing                                                                                                                                          |
| `media.processor.intervalMs`               | integer | > 0                                                                                     | `30000`                      | Applied live (media processing interval)            | Dispatch frequency for processing loop (ms)                                                                                                                                 |
| `tpm.types.media_processor.share`          | double  | (0,1]                                                                                   | `1.0`                        | Applied live (thread pool share)                    | TPM share for media processor tasks                                                                                                                                         |
| `tpm.types.image_processor.share`          | double  | (0,1]                                                                                   | `1.0`                        | Applied live (reserved for future use)              | Reserved: TPM share for image processor                                                                                                                                     |
| `tpm.types.audio_processor.share`          | double  | (0,1]                                                                                   | `1.0`                        | Applied live (reserved for future use)              | Reserved: TPM share for audio processor                                                                                                                                     |
| `tpm.types.video_processor.share`          | double  | (0,1]                                                                                   | `1.0`                        | Applied live (reserved for future use)              | Reserved: TPM share for video processor                                                                                                                                     |
| `media.image.timeoutMs`                    | integer | > 0                                                                                     | `30000`                      | Applied to new tasks                                | Per-image processing timeout (ms)                                                                                                                                           |
| `media.image.retry.enabled`                | boolean | `true` \| `false`                                                                       | `true`                       | Applied to new tasks                                | Enable retries for transient failures                                                                                                                                       |
| `media.image.retry.maxAttempts`            | integer | ≥ 0                                                                                     | `2`                          | Applied to new tasks                                | Number of retry attempts (excludes first try)                                                                                                                               |
| `media.image.retry.baseDelayMs`            | integer | ≥ 0                                                                                     | `500`                        | Applied to new tasks                                | Base delay (ms) for exponential backoff                                                                                                                                     |
| `media.image.fast.thumbSize`               | integer | > 0                                                                                     | `256`                        | Applied to new tasks                                | Thumbnail size used by FAST pipeline                                                                                                                                        |
| `media.image.balanced.resizeLongEdge`      | integer | > 0                                                                                     | `1024`                       | Applied to new tasks                                | Resize long edge used by BALANCED pipeline                                                                                                                                  |
| `media.image.balanced.maxKeypoints`        | integer | > 0                                                                                     | `1000`                       | Applied to new tasks                                | Max keypoints kept by BALANCED pipeline                                                                                                                                     |
| `media.image.quality.onnx.modelPath`       | string  | valid path                                                                              | `models/clip-RN50.onnx`      | Applied to new tasks                                | ONNX model path for QUALITY pipeline                                                                                                                                        |
| `media.image.quality.onnx.inputSize`       | integer | > 0                                                                                     | `224`                        | Applied to new tasks                                | ONNX input size (pixels) for QUALITY pipeline                                                                                                                               |
| `media.image.quality.embeddingDim`         | integer | > 0                                                                                     | `512`                        | Applied to new tasks                                | Output embedding dimension for QUALITY pipeline                                                                                                                             |
| `duplicates.finder.enabled`                | boolean | `true` \| `false`                                                                       | `true`                       | Applied live                                        | Enable duplicate detection service                                                                                                                                          |
| `duplicates.finder.intervalMs`             | integer | > 0                                                                                     | `3600000`                    | Applied live (1 hour default)                       | Duplicate finder execution interval (ms)                                                                                                                                    |
| `duplicates.finder.batchSize`              | integer | > 0                                                                                     | `1000`                       | Applied live                                        | Files to process per duplicate detection batch                                                                                                                              |
| `duplicates.finder.maxGroupSize`           | integer | > 0                                                                                     | `100`                        | Applied live                                        | Maximum duplicates per group                                                                                                                                                |
| `tpm.types.duplicate_finder.share`         | double  | (0,1]                                                                                   | `1.0`                        | Applied live (thread pool share)                    | TPM share for duplicate finder tasks                                                                                                                                        |
| `duplicates.fast.threshold`                | double  | 0.0-1.0                                                                                 | `0.92`                       | Applied live                                        | pHash similarity threshold for FAST mode (0.90-0.95 recommended)                                                                                                            |
| `duplicates.fast.minHashSize`              | integer | 64, 128, 256                                                                            | `64`                         | Applied live                                        | Minimum pHash size in bits (future expansion support)                                                                                                                       |
| `duplicates.balanced.threshold`            | double  | 0.0-1.0                                                                                 | `0.35`                       | Applied live                                        | Feature match ratio threshold for BALANCED mode (0.30-0.40 recommended)                                                                                                     |
| `duplicates.balanced.minGoodMatches`       | integer | 0-100                                                                                   | `15`                         | Applied live                                        | Minimum good feature matches required for duplicate (0=disabled)                                                                                                            |
| `duplicates.balanced.ratioTestThreshold`   | double  | 0.0-1.0                                                                                 | `0.75`                       | Applied live                                        | Lowe's ratio test threshold for feature matching quality                                                                                                                    |
| `duplicates.threshold.min`                 | double  | 0.0-1.0                                                                                 | `0.92`                       | Applied live (triggers reprocess if decreased)      | Minimum similarity threshold for adding files to groups (0.90-0.95 recommended). Controls cross-group checking aggressiveness - narrower ranges enable more optimization.   |
| `duplicates.threshold.max`                 | double  | 0.0-1.0                                                                                 | `0.96`                       | Applied live                                        | Maximum similarity threshold for representative swaps and cross-group checking (0.94-0.98 recommended). Files with scores below this may be moved to better-matched groups. |
| `duplicates.metadata.filtering.enabled`    | boolean | `true` \| `false`                                                                       | `true`                       | Applied live                                        | Enable metadata pre-filtering before similarity calculation                                                                                                                 |
| `duplicates.metadata.aspectRatioTolerance` | double  | 0.0-1.0                                                                                 | `0.10`                       | Applied live                                        | Max aspect ratio difference allowed (0.10 = 10%)                                                                                                                            |
| `duplicates.metadata.dimensionTolerance`   | double  | 0.0-1.0                                                                                 | `0.20`                       | Applied live                                        | Max dimension difference allowed (0.20 = 20%)                                                                                                                               |
| `duplicates.metadata.fileSizeTolerance`    | double  | 0.0-5.0                                                                                 | `0.50`                       | Applied live                                        | Max file size difference allowed (0.50 = 50%)                                                                                                                               |
| `duplicates.metadata.requireSameFormat`    | boolean | `true` \| `false`                                                                       | `false`                      | Applied live                                        | If true, only compare files with same extension                                                                                                                             |
| `duplicates.representative.strategy`       | string  | `size_then_age` \| `age_then_size`                                                      | `size_then_age`              | Applied live                                        | Strategy for selecting group representative                                                                                                                                 |

Notes:

- `logging.level` synonyms are mapped internally for Poco compatibility: `info → information`, `warn → warning`.
- Unknown keys are ignored unless used by the application.
- Factory-created properties are automatically generated when using `ConfigManagerFactory`.

## Example

```yaml
# Server configuration
server.host: 0.0.0.0
server.port: 8080
server.name: Media Deduplication Server
server.mode: FAST # FAST | BALANCED | QUALITY
server.processName: media_dedup_server
server.instanceCheck.enabled: true
server.instanceCheck.bufferSize: 128
server.max_connections: 100
server.timeout: 30.0

# Database configuration
database.path: data/dedup_server.db
database.session.acquireTimeoutMs: 3000
database.session.acquireBackoffMs: 50
database.session.poolMin: 8
database.session.poolMax: 32

# HTTP Server Thread Pool
server.http.threadPool.maxThreads: auto # auto or integer
server.http.threadPool.maxQueued: 50

# Cache configuration
cache.disk.location: cache
cache.disk.size_limit_mb: 2048
cache.disk.clearOnStartup: true # Clear transcoding cache (temporary files)
cache.thumbnail.location: cache/thumbnails
cache.thumbnail.size_limit_mb: 512
cache.thumbnail.clearOnStartup: false # Preserve thumbnails across restarts

# Logging configuration
logging.level: info # trace | debug | info | warn | error (case-insensitive)
logging.enable_console: true
logging.enable_file: false

# Files manager configuration
files.manager.enabled: true
files.manager.scan.intervalMs: 500

# Scheduler configuration
scheduler.jitter.enabled: false
scheduler.jitter.percent: 0
scheduler.drift.mode: anchored
scheduler.drift.maxDriftMs: 60000
scheduler.backoff.enabled: true
scheduler.backoff.initialMs: 1000
scheduler.backoff.multiplier: 2.0
scheduler.backoff.maxMs: 30000
scheduler.backoff.jitterPercent: 10

# Thread Pool Manager configuration
tpm.pool.max: auto
tpm.killTimeoutMs: 10000
tpm.thread.idleTimeoutSeconds: 120
tpm.types.fileScan.share: 1.0

# Debug configuration
debug.enabled: false
debug.verbose: false

# File monitoring configuration
file_monitoring.enabled: true
file_monitoring.interval: 500

# Validation configuration
validation.enabled: true
validation.strict: false

# Media category configuration
# Media Processor
media.processor.enabled: true
media.processor.intervalMs: 30000

# TPM thread type shares (consolidated naming: tpm.types.<type>.share)
tpm.types.media_processor.share: 1.0
tpm.types.image_processor.share: 1.0 # Reserved for future use
tpm.types.audio_processor.share: 1.0 # Reserved for future use
tpm.types.video_processor.share: 1.0 # Reserved for future use

# Images
media.images.jpg: true
media.images.jpeg: true
media.images.png: true
media.images.bmp: true
media.images.gif: true
media.images.tiff: true
media.images.webp: true
media.images.jp2: true
media.images.ppm: true
media.images.pgm: true
media.images.pbm: true
media.images.pnm: true
media.images.exr: true
media.images.hdr: true

# Video
media.video.mp4: true
media.video.avi: true
media.video.mov: true
media.video.mkv: true
media.video.wmv: true
media.video.flv: true
media.video.webm: true
media.video.m4v: true
media.video.mpg: true
media.video.mpeg: true
media.video.ts: true
media.video.mts: true
media.video.m2ts: true
media.video.ogv: true

# Audio
media.audio.mp3: true
media.audio.wav: true
media.audio.flac: true
media.audio.ogg: true
media.audio.m4a: true
media.audio.aac: true
media.audio.opus: true
media.audio.wma: true
media.audio.aiff: true
media.audio.alac: true
media.audio.amr: true
media.audio.au: true

# Raw Images (subcategory under images)
media.images.raw.cr2: true
media.images.raw.nef: true
media.images.raw.arw: true
media.images.raw.dng: true
media.images.raw.raf: true
media.images.raw.rw2: true
media.images.raw.orf: true
media.images.raw.pef: true
media.images.raw.srw: true
media.images.raw.kdc: true
media.images.raw.dcr: true
media.images.raw.mos: true
media.images.raw.mrw: true
media.images.raw.raw: true
media.images.raw.bay: true
media.images.raw.3fr: true
media.images.raw.fff: true
media.images.raw.mef: true
media.images.raw.iiq: true
media.images.raw.rwz: true
media.images.raw.nrw: true
media.images.raw.rwl: true
```

## Live updates

- Changing `server.host` or `server.port` triggers an automatic web server restart with the new binding (with console log old → new).
- Changing `logging.level` applies immediately to the server and all component loggers.
- Changing `server.mode` applies immediately and influences processing behavior:
  - FAST: prioritize speed, minimal metadata, quick duplicate checks
  - BALANCED: trade-off between speed and quality
  - QUALITY: most comprehensive metadata and de-duplication passes
- Changing `server.instanceCheck.enabled` or `server.instanceCheck.bufferSize` applies immediately to instance checking behavior.
- Changing `database.session.acquireTimeoutMs` or `database.session.acquireBackoffMs` applies immediately to the database session acquisition timing.
- Changing `database.session.poolMin` or `database.session.poolMax` requires a server restart (session pool is initialized once at startup).
- Changing `files.manager.enabled` or `files.manager.scan.intervalMs` applies immediately to file scanning behavior.
- Changing `media.processor.enabled`, `media.processor.intervalMs`, or `tpm.types.*_processor.share` applies immediately to media processing behavior.
- Changing `scheduler.jitter.enabled`, `scheduler.jitter.percent`, `scheduler.drift.mode`, or `scheduler.drift.maxDriftMs` applies immediately to scheduler behavior.
- Changing `scheduler.backoff.*` properties applies immediately to scheduler backoff behavior.
- Changing `tpm.pool.max` applies immediately; if lowered, TPM stops starting new tasks until concurrency drops below the new cap.
- Changing `tpm.thread.idleTimeoutSeconds` recreates the thread pool with the new idle timeout.
- Changing `tpm.types.<name>.share` updates the per-type allowance used for scheduling new tasks.
- Changing any `media.*` property applies immediately to file scanning and processing behavior:
  - `media.images.*` properties control which image formats are processed during file scanning
  - `media.images.raw.*` properties control which raw image formats are processed during file scanning
  - `media.video.*` properties control which video formats are processed during file scanning
  - `media.audio.*` properties control which audio formats are processed during file scanning
  - Setting a media type to `false` will exclude files of that type from processing
  - Setting a media type to `true` will include files of that type in processing
- API writes persist back to YAML; existing files are not replaced, only values are updated.

## HTTP API (OpenAPI)

- OpenAPI spec: `GET /api/openapi.json`
- Read all config: `GET /api/v1/config`
- Read one key: `GET /api/v1/config/{key}`
- Update key: `PUT /api/v1/config/{key}` with JSON body `{ "value": "..." }`
- Reload from disk: `POST /api/v1/config/reload`
- Status: `GET /api/v1/config/status`
- TPM status: `GET /api/v1/tpm/status`

### Update examples

```bash
# Change logging level
curl -X PUT http://localhost:8080/api/v1/config/logging.level \
  -H 'Content-Type: application/json' \
  -d '{"value":"debug"}'

# Change port (server restarts)
curl -X PUT http://localhost:8080/api/v1/config/server.port \
  -H 'Content-Type: application/json' \
  -d '{"value":"9090"}'

# Set TPM pool max to fixed size
curl -X PUT http://localhost:8080/api/v1/config/tpm.pool.max \
  -H 'Content-Type: application/json' \
  -d '{"value":"4"}'

# Set a per-type share
curl -X PUT http://localhost:8080/api/v1/config/tpm.types.transcode.share \
  -H 'Content-Type: application/json' \
  -d '{"value":"0.4"}'
```

## Behavior on startup

- If `config/config.yaml` exists, it is loaded as-is; only missing keys are seeded with defaults.
- If no config is present, a new file is created with sane defaults.
- The database file defined by `database.path` is created if missing (parent directories are auto-created).

## User Settings

Key/value settings persisted in SQLite.

- Table: `user_settings (key TEXT PRIMARY KEY, value TEXT NOT NULL)`
- Endpoints:
  - `GET /api/v1/user-settings` – list all settings
  - `GET /api/v1/user-settings/{key}` – get one setting
  - `PUT /api/v1/user-settings/{key}` – create/update with body `{ "value": "..." }`
  - `DELETE /api/v1/user-settings/{key}` – delete setting

---

# Observable Configuration System

This directory contains a sophisticated configuration management system that provides **bidirectional observable properties** for your media deduplication server.

## 🎯 **What This System Provides**

### **Bidirectional Updates**

- **Programmatic changes** → **File updates** automatically
- **File changes** → **Property updates** automatically
- **Real-time synchronization** between code and configuration files

### **Observable Properties**

- **Watch for changes** in configuration values
- **Execute callbacks** when properties change
- **Validate values** before applying changes
- **Thread-safe operations** with proper locking

### **Log Level Management**

- **Specialized LogLevel enum** with validation
- **String conversion** for YAML/JSON files
- **Level manipulation** (increase/decrease, set to extremes)
- **Level checking** (isTraceEnabled, isDebugEnabled, etc.)

## 🏗️ **Architecture Overview**

```
┌─────────────────┐    ┌──────────────────┐    ┌─────────────────┐
│   Code/App      │◄──►│ Observable Props │◄──►│  Config Files   │
│                 │    │                  │    │                 │
│ setLogLevel()   │    │ Change Callbacks │    │ YAML/JSON      │
│ getLogLevel()   │    │ Validation       │    │ Auto-reload    │
└─────────────────┘    └──────────────────┘    └─────────────────┘
```

## 📁 **File Structure**

```
config/
├── logging.yaml                    # Logging configuration file
├── observable_property.hpp         # Generic observable property template
├── log_level.hpp                   # Log level enum and utilities
├── observable_log_level.hpp        # Specialized log level property
├── observable_config_manager.hpp   # Main configuration manager
├── examples/
│   └── config_demo.cpp            # Demo program
└── README.md                       # This file
```

## 🚀 **Quick Start**

### **1. Basic Usage**

```cpp
#include "config/observable_config_manager.hpp"
#include "config/log_level.hpp"

using namespace MediaDedup;

// Create configuration manager
ObservableConfigManager config("config/logging.yaml");

// Initialize and load
config.initialize();
config.loadConfiguration();

// Get log level property
auto log_level = config.getLogLevelProperty("log_level");
if (log_level) {
    // Set up change callback
    log_level->setChangeCallback([](const LogLevel& old_level, const LogLevel& new_level) {
        std::cout << "Log level changed from " << logLevelToString(old_level)
                  << " to " << logLevelToString(new_level) << std::endl;
        // Update your logging system here
    });

    // Change log level programmatically
    log_level->setValue(LogLevel::DEBUG);
    // This automatically updates the config file!
}
```

### **2. File Monitoring**

```cpp
// Enable automatic file reloading
config.setAutoReload(true);
config.setReloadInterval(std::chrono::milliseconds(1000));

// Set up file change callback
config.setFileChangeCallback([](const std::string& file_path) {
    std::cout << "Configuration file changed: " << file_path << std::endl;
});

// Set up property change callback
config.setPropertyChangeCallback([](const std::string& key,
                                   const std::string& old_value,
                                   const std::string& new_value) {
    std::cout << "Property " << key << " changed: "
              << old_value << " -> " << new_value << std::endl;
});
```

### **3. Log Level Operations**

```cpp
auto log_level = config.getLogLevelProperty("log_level");

// Check if specific levels are enabled
if (log_level->isDebugEnabled()) {
    std::cout << "Debug logging is enabled\n";
}

// Manipulate log levels
log_level->increaseLevel();        // Make more verbose
log_level->decreaseLevel();        // Make less verbose
log_level->setMostVerbose();       // Set to TRACE
log_level->setLeastVerbose();      // Set to FATAL only

// Get level information
std::cout << "Current level: " << log_level->getValueAsString() << "\n";
std::cout << "Description: " << log_level->getCurrentLevelDescription() << "\n";
```

## 📊 **Configuration File Format**

The system supports YAML configuration files. Here's an example:

```yaml
# Log Level Configuration
log_level: "info" # trace, debug, info, warn, error, fatal

# Log Output Configuration
log_output:
  console: true
  file: true
  syslog: false

# File Logging Configuration
file_logging:
  path: "logs/media_dedup.log"
  max_size: 10485760
  max_files: 5
  rotation: "daily"
  compress: true
```

## 🔧 **Advanced Features**

### **Validation Callbacks**

```cpp
auto property = config.getProperty<bool>("enable_debug");
if (property) {
    property->setValidationCallback([](const bool& value) -> bool {
        // Only allow debug in development mode
        return !isProductionEnvironment() || !value;
    });
}
```

### **Custom Property Types**

```cpp
// Create custom observable property
auto custom_prop = std::make_shared<ObservableProperty<std::string>>(
    "custom_setting", "default_value", "Custom configuration setting"
);

// Set up callbacks
custom_prop->setChangeCallback([](const std::string& old_val, const std::string& new_val) {
    std::cout << "Custom setting changed: " << old_val << " -> " << new_val << "\n";
});
```

### **Thread Safety**

All operations are thread-safe with proper mutex locking:

```cpp
// Multiple threads can safely access properties
std::thread t1([&config]() {
    config.setPropertyValue("log_level", LogLevel::DEBUG);
});

std::thread t2([&config]() {
    auto level = config.getPropertyValue("log_level", LogLevel::INFO);
    std::cout << "Thread 2 sees level: " << logLevelToString(level) << "\n";
});

t1.join();
t2.join();
```

## 🧪 **Running the Demo**

```bash
# Build the demo
cd examples
g++ -std=c++17 -I../.. config_demo.cpp -o config_demo

# Run the demo
./config_demo
```

The demo will:

1. Load configuration from `config/logging.yaml`
2. Set up change callbacks
3. Demonstrate programmatic changes
4. Monitor for file changes
5. Show bidirectional synchronization

## ⚠️ **Important Notes**

### **File Permissions**

- Ensure the configuration file is writable by your application
- The system will automatically create backup files if needed

### **File Format**

- Currently supports YAML format
- File must be valid YAML syntax
- Invalid files will trigger validation errors

### **Performance**

- File monitoring runs in a separate thread
- Default reload interval is 1 second
- Adjust based on your needs

### **Error Handling**

- Always check return values from initialization methods
- Use `isValid()` to check configuration validity
- Check `getValidationErrors()` for detailed error information

## 🔮 **Future Enhancements**

- **JSON configuration** support
- **Environment variable** overrides
- **Configuration encryption** for sensitive data
- **Remote configuration** via HTTP/HTTPS
- **Configuration validation** schemas
- **Hot reloading** without restart

## 🤝 **Contributing**

When adding new configuration properties:

1. **Use appropriate types** (string, int, bool, double, LogLevel)
2. **Add validation** where appropriate
3. **Document the property** in configuration files
4. **Add to demo** if it's a key feature
5. **Test bidirectional updates** thoroughly

## 📚 **API Reference**

See the header files for complete API documentation:

- `ObservableProperty<T>` - Generic observable property
- `ObservableLogLevel` - Specialized log level property
- `ObservableConfigManager` - Main configuration manager
- `LogLevel` - Log level enumeration

---

**This system provides a robust, flexible foundation for configuration management in your media deduplication server! 🎉**

---

## Configuration Rules and Playbook

This document defines the rules and the exact steps to add a new configuration property to the Unified Observable Configuration System. It is written for both humans and generative AI agents.

The system is centered around `UnifiedObservableConfigManager` which provides:

- Type-safe properties with `std::any` under the hood
- YAML file persistence and auto-reload
- Event-driven change notifications
- Pluggable and built-in validation

Key components (see `src/config` and `include/config`):

- `UnifiedObservableConfigManager` orchestrates everything
- `ConfigProperty` and `ObservableConfigProperty` represent properties
- `ConfigPropertyManager` stores properties
- `ConfigFileManager` handles YAML I/O
- `ConfigFileMonitor` watches for file changes (auto-reload)
- `ConfigEventManager` emits `ConfigChangeEvent`
- `ConfigValidator` validates properties and full configurations
- `ConfigTypeConverter` converts between `std::any`/native types/strings
- `ConfigManagerFactory` creates default properties and recommended wiring

### Quick checklist (TL;DR)

- Define a clear key name using dot-separated namespaces (e.g., `server.port`, `files.manager.scan.intervalMs`).
- Choose a concrete C++ type supported by the system.
- Add the property to `ConfigManagerFactory` with a default and description.
- If the subsystem needs live reaction, subscribe to config change events and handle the key.
- Add validation: built-in if covered; otherwise register a custom validator for the key.
- Update `config/config.yaml` if you want a checked-in default override; ensure it matches the factory default or intentionally differs.
- Document the new key in `config/CONFIGURATION_REFERENCE.md`.
- Add unit tests under `tests/` to cover default, parsing, validation, and event reaction.

### Supported property types

The system supports standard scalar types and a few collections. Commonly used:

- string-like: `std::string`
- integers: `int`, `long`, `long long`, `unsigned int`, `unsigned long`, `unsigned long long`
- floating-point: `float`, `double` (stored and stringified with fixed 6 decimal precision)
- boolean: `bool`
- collections: `std::vector<std::string>`

All values are stored as `std::any` inside properties. Use `ConfigTypeConverter` for safe conversions when needed.

### Step-by-step: Adding a new property

1. Pick the key and type

- Use a dot-separated namespace that fits existing conventions.
- Examples: `filesservice.scan.recursive`, `scheduler.jobs.fileScan.intervalMs`, `logging.level`.

2. Define defaults in the factory
   Add the property to `ConfigManagerFactory` (preferred) so the whole system sees a consistent default and description.

Example (add to `src/config/config_manager_factory.cpp` in the appropriate section):

```cpp
// Example: add a throttle toggle for file scans
manager->createProperty<bool>(
    "files.manager.throttle.enabled",
    false,
    "Enable throttling for files manager scans");

manager->createProperty<int>(
    "files.manager.throttle.maxFilesPerRun",
    10000,
    "Max files to process per run when throttling is enabled");
```

3. (Optional) Include in checked-in config file
   If you want the default visible/editable in the repo config, add it to `config/config.yaml`. Keep it in sync with the factory default unless there's a deliberate difference.

4. React to changes (event-driven)
   Subscribe to config changes and handle your key to apply live updates. Use the manager’s event API:

```cpp
configManager->subscribeToConfigChanges([
    /* capture subsystems or weak refs as needed */
](const MediaDedup::ConfigChangeEvent &evt) {
    if (evt.key == "files.manager.throttle.enabled") {
        // evt.new_value is std::any; convert safely
        bool enabled = false;
        try { enabled = std::any_cast<bool>(evt.new_value); } catch (...) {}
        myFilesManager->setThrottleEnabled(enabled);
    }
});
```

Notes:

- Event fields: `key`, `old_value`, `new_value`, `source` ("programmatic" | "file" | "default"), `is_file_update`.
- Keep callbacks lightweight; they run within the event emission loop.
- For bulk updates, consider batching your reactions.

5. Add validation
   Use built-in validation when your key falls under existing domains (server/logging/database/file scanner). Otherwise, register a custom validator:

```cpp
configManager->registerValidationCallback(
    "files.manager.throttle.maxFilesPerRun",
    [](const std::string &key, const std::any &value) -> bool {
        try {
            int v = std::any_cast<int>(value);
            return v > 0 && v <= 2'000'000; // reasonable bounds
        } catch (...) {
            return false;
        }
    }
);
```

You can enable/disable validation globally:

```cpp
configManager->setValidationEnabled(true);
```

6. Persisting and loading

- Programmatic changes: `configManager->setPropertyValue<T>(key, value);`
- File-originated changes: edit YAML; the monitor auto-reloads if enabled.
- Explicit persistence: `configManager->triggerSave();` (usually not required for single changes).

7. Auto-reload (reactive from file changes)

- File monitoring is on by default if the manager was constructed with monitoring enabled.
- Control at runtime:

```cpp
configManager->setAutoReload(true); // or false
configManager->setReloadInterval(std::chrono::milliseconds(1000));
```

On file change, the manager calls `reloadConfiguration()` and emits config change events.

### Coding patterns and APIs to use

Creating or accessing properties:

```cpp
// Create with default
auto prop = configManager->createProperty<int>("app.cache.maxEntries", 1000, "Cache size limit");

// Read with fallback
int maxEntries = configManager->getPropertyValue<int>("app.cache.maxEntries", 1000);

// Update (emits event)
configManager->setPropertyValue<int>("app.cache.maxEntries", 500);
```

Event subscription:

```cpp
configManager->subscribeToConfigChanges(
    [](const MediaDedup::ConfigChangeEvent &evt) {
        if (evt.key == "logging.level") {
            // convert std::any to std::string safely
            std::string level;
            try { level = std::any_cast<std::string>(evt.new_value); } catch (...) { return; }
            // apply to logger
            applyLoggingLevel(level);
        }
    }
);
```

Property-level conversion helpers (when you already have a `ConfigProperty`):

```cpp
auto p = configManager->getProperty<std::any>("app.cache.maxEntries");
if (p) {
    int v = 0;
    try { v = std::any_cast<int>(p->getValue()); } catch (...) {}
}
```

### Naming and structure conventions

- Use lowercase namespaces separated by dots: `domain.subdomain.name`.
- Prefer descriptive keys over abbreviations.
- Align with existing domains when possible: `server.*`, `logging.*`, `database.*`, `scheduler.*`, `filesservice.*`, `files.manager.*`, `tpm.*`.
- For per-job scheduler overrides, prefer `scheduler.jobs.<jobId>.<setting>`.

### Validation conventions

- Favor narrow, clear ranges and enums.
- Leverage existing built-in validators by placing keys under recognized domains.
- Use `registerValidationCallback` for bespoke constraints.
- Keep validation pure (no side-effects). Return `true` for valid, `false` for invalid; detailed errors are tracked internally by `ConfigValidator`.

### Reactivity policies (recommended)

- For components needing live updates, subscribe to change events and apply changes immediately.
- If a component polls configuration periodically, ensure it reads via `getPropertyValue<T>` each cycle.
- Avoid long/blocking work inside change callbacks; dispatch to background threads if necessary.
- For frequently updated properties, consider idempotent handlers and deduplicate if necessary.

### YAML and persistence

- YAML is the source of truth on disk; programmatic changes are persisted via `saveConfiguration()`.
- Keys not present in YAML will be created on first save with their current values.
- Unknown types fall back to string storage; prefer explicit typed defaults in the factory.

### Testing guidance

- Add unit tests that:
  - Verify default values from the factory
  - Parse YAML with valid and invalid values
  - Confirm validation accepts good values and rejects bad values
  - Assert change events fire with correct `old_value`, `new_value`, `source`, and `is_file_update`
  - Exercise auto-reload by touching the YAML file

### Anti-patterns (avoid)

- Creating properties ad-hoc in scattered code; use the factory.
- Using raw `std::any_cast` without try/catch in event handlers.
- Doing heavy work or blocking I/O directly inside change callbacks.
- Adding new keys to YAML without adding them to the factory and documentation.

### Reference: Relevant APIs (signatures)

- Property creation/access: see `include/config/unified_observable_config.hpp` and `include/config/config_property_manager.hpp`.
- Events: `subscribeToConfigChanges`, `emitConfigChangeEvent`, `ConfigChangeEvent`.
- Validation: `ConfigValidator::registerValidationCallback`, `setValidationEnabled`.
- Persistence: `saveConfiguration`, `triggerSave`, `setAutoReload`, `setReloadInterval`.

### Example end-to-end addition

1. Factory defaults:

```cpp
manager->createProperty<std::string>(
    "app.theme",
    "system",
    "UI theme: light|dark|system");
```

2. Custom validation:

```cpp
configManager->registerValidationCallback("app.theme",
    [](const std::string&, const std::any &v){
        try { auto s = std::any_cast<std::string>(v);
              return s == "light" || s == "dark" || s == "system"; }
        catch (...) { return false; }
    });
```

3. React to changes:

```cpp
configManager->subscribeToConfigChanges([](const ConfigChangeEvent &evt){
    if (evt.key == "app.theme") applyThemeSafely(evt);
});
```

4. Document in `CONFIGURATION_REFERENCE.md` and optionally add to `config.yaml`.

---

By following this playbook, new properties will be consistent, validated, reactive, and well-documented across the system.

