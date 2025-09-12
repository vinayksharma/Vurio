# Configuration Reference

This project uses a YAML configuration file that is auto-loaded and monitored at runtime. Values changed on disk are picked up automatically.

- File path: `config/config.yaml` (fallback: `./config.yaml`)
- Auto-reload interval: 1s
- Existing files are not overwritten at startup

## Properties

| Key                                 | Type    | Allowed values                                              | Default                      | Live effect                                       |
| ----------------------------------- | ------- | ----------------------------------------------------------- | ---------------------------- | ------------------------------------------------- |
| `server.host`                       | string  | Any valid hostname/IP. `0.0.0.0` binds all                  | `0.0.0.0`                    | Restart web server on change                      |
| `server.port`                       | integer | 1–65535 (must be non-zero)                                  | `8080`                       | Restart web server on change                      |
| `server.name`                       | string  | Any non-empty string                                        | `Media Deduplication Server` | None (log-only)                                   |
| `server.mode`                       | string  | `FAST` \| `BALANCED` \| `QUALITY`                           | `FAST`                       | Applied live; drives processing/de-dup strategy   |
| `server.processName`                | string  | Process name for instance checking                          | `media_dedup_server`         | Applied live                                      |
| `server.instanceCheck.enabled`      | boolean | `true` \| `false`                                           | `true`                       | Applied live                                      |
| `server.instanceCheck.bufferSize`   | integer | > 0                                                         | `128`                        | Applied live                                      |
| `database.path`                     | string  | File path; parent dirs will be auto-created                 | `data/dedup_server.db`       | New path is honored on next DB (re)initialization |
| `database.session.acquireTimeoutMs` | integer | > 0                                                         | `3000`                       | Applied live (DB session acquire timeout)         |
| `database.session.acquireBackoffMs` | integer | > 0                                                         | `50`                         | Applied live (retry backoff while waiting)        |
| `logging.level`                     | string  | Case-insensitive: `trace`, `debug`, `info`, `warn`, `error` | `info`                       | Applied live                                      |
| `files.manager.enabled`             | boolean | `true` \| `false`                                           | `true`                       | Applied live                                      |
| `files.manager.scan.intervalMs`     | integer | > 0                                                         | `500`                        | Applied live                                      |
| `scheduler.jitter.enabled`          | boolean | `true` \| `false`                                           | `false`                      | Applied live                                      |
| `scheduler.jitter.percent`          | integer | 0-100                                                       | `0`                          | Applied live                                      |
| `scheduler.drift.mode`              | string  | `anchored` \| `floating`                                    | `anchored`                   | Applied live                                      |
| `scheduler.drift.maxDriftMs`        | integer | > 0                                                         | `60000`                      | Applied live                                      |
| `scheduler.backoff.enabled`         | boolean | `true` \| `false`                                           | `true`                       | Applied live                                      |
| `scheduler.backoff.initialMs`       | integer | > 0                                                         | `1000`                       | Applied live                                      |
| `scheduler.backoff.multiplier`      | number  | > 1.0                                                       | `2.0`                        | Applied live                                      |
| `scheduler.backoff.maxMs`           | integer | > 0                                                         | `30000`                      | Applied live                                      |
| `scheduler.backoff.jitterPercent`   | integer | 0-100                                                       | `10`                         | Applied live                                      |
| `tpm.pool.max`                      | string  | `auto` or integer                                           | `auto`                       | Applied live; decrease is gradual                 |
| `tpm.killTimeoutMs`                 | integer | > 0                                                         | `10000`                      | Drain timeout on shutdown                         |
| `tpm.types.<name>.share`            | number  | (0,1]                                                       | `1.0`                        | Per-type slice; honor-based                       |
| `server.max_connections`            | integer | > 0                                                         | `100`                        | Applied live                                      |
| `server.timeout`                    | number  | > 0.0                                                       | `30.0`                       | Applied live                                      |
| `logging.enable_console`            | boolean | `true` \| `false`                                           | `true`                       | Applied live                                      |
| `logging.enable_file`               | boolean | `true` \| `false`                                           | `false`                      | Applied live                                      |
| `debug.enabled`                     | boolean | `true` \| `false`                                           | `false`                      | Applied live                                      |
| `debug.verbose`                     | boolean | `true` \| `false`                                           | `false`                      | Applied live                                      |
| `file_monitoring.enabled`           | boolean | `true` \| `false`                                           | `true`                       | Applied live                                      |
| `file_monitoring.interval`          | integer | > 0                                                         | `500`                        | Applied live                                      |
| `validation.enabled`                | boolean | `true` \| `false`                                           | `true`                       | Applied live                                      |
| `validation.strict`                 | boolean | `true` \| `false`                                           | `false`                      | Applied live                                      |

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
- Changing `files.manager.enabled` or `files.manager.scan.intervalMs` applies immediately to file scanning behavior.
- Changing `scheduler.jitter.enabled`, `scheduler.jitter.percent`, `scheduler.drift.mode`, or `scheduler.drift.maxDriftMs` applies immediately to scheduler behavior.
- Changing `scheduler.backoff.*` properties applies immediately to scheduler backoff behavior.
- Changing `tpm.pool.max` applies immediately; if lowered, TPM stops starting new tasks until concurrency drops below the new cap.
- Changing `tpm.types.<name>.share` updates the per-type allowance used for scheduling new tasks.
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
