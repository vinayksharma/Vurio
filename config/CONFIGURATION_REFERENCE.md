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
| `database.path`                     | string  | File path; parent dirs will be auto-created                 | `data/dedup_server.db`       | New path is honored on next DB (re)initialization |
| `database.session.acquireTimeoutMs` | integer | > 0                                                         | `3000`                       | Applied live (DB session acquire timeout)         |
| `database.session.acquireBackoffMs` | integer | > 0                                                         | `50`                         | Applied live (retry backoff while waiting)        |
| `logging.level`                     | string  | Case-insensitive: `trace`, `debug`, `info`, `warn`, `error` | `info`                       | Applied live                                      |
| `server.mode`                       | string  | `FAST` \| `BALANCED` \| `QUALITY`                           | `FAST`                       | Applied live; drives processing/de-dup strategy   |

Notes:

- `logging.level` synonyms are mapped internally for Poco compatibility: `info → information`, `warn → warning`.
- Unknown keys are ignored unless used by the application.

## Example

```yaml
server.host: 0.0.0.0
server.port: 8080
server.name: Media Deduplication Server

# Database
database.path: data/dedupdb.db
database.session.acquireTimeoutMs: 3000
database.session.acquireBackoffMs: 50

# Logging
logging.level: info # trace | debug | info | warn | error (case-insensitive)

# Server processing mode
server.mode: FAST # FAST | BALANCED | QUALITY
```

## Live updates

- Changing `server.host` or `server.port` triggers an automatic web server restart with the new binding (with console log old → new).
- Changing `logging.level` applies immediately to the server and root logger.
- Changing `server.mode` applies immediately and influences processing behavior:
  - FAST: prioritize speed, minimal metadata, quick duplicate checks
  - BALANCED: trade-off between speed and quality
  - QUALITY: most comprehensive metadata and de-duplication passes
- Changing `database.session.acquireTimeoutMs` or `database.session.acquireBackoffMs` applies immediately to the database session acquisition timing.
- API writes persist back to YAML; existing files are not replaced, only values are updated.

## HTTP API (OpenAPI)

- OpenAPI spec: `GET /api/openapi.json`
- Read all config: `GET /api/v1/config`
- Read one key: `GET /api/v1/config/{key}`
- Update key: `PUT /api/v1/config/{key}` with JSON body `{ "value": "..." }`
- Reload from disk: `POST /api/v1/config/reload`
- Status: `GET /api/v1/config/status`

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
