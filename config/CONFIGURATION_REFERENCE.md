# Configuration Reference

This project uses a YAML configuration file that is auto-loaded and monitored at runtime. Values changed on disk are picked up automatically.

- File path: `config/config.yaml` (fallback: `./config.yaml`)
- Auto-reload interval: 1s
- Existing files are not overwritten at startup

## Properties

| Key             | Type    | Allowed values                                              | Default                      | Live effect                                       |
| --------------- | ------- | ----------------------------------------------------------- | ---------------------------- | ------------------------------------------------- |
| `server.host`   | string  | Any valid hostname/IP. `0.0.0.0` binds all                  | `0.0.0.0`                    | Restart web server on change                      |
| `server.port`   | integer | 1–65535 (must be non-zero)                                  | `8080`                       | Restart web server on change                      |
| `server.name`   | string  | Any non-empty string                                        | `Media Deduplication Server` | None (log-only)                                   |
| `database.path` | string  | File path; parent dirs will be auto-created                 | `data/dedup_server.db`       | New path is honored on next DB (re)initialization |
| `logging.level` | string  | Case-insensitive: `trace`, `debug`, `info`, `warn`, `error` | `info`                       | Applied live                                      |

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

# Logging
logging.level: info # trace | debug | info | warn | error (case-insensitive)
```

## Live updates

- Changing `server.host` or `server.port` in the file triggers:
  - Console log: old → new
  - A web server restart with the new binding
- Changing `logging.level` applies immediately to the server and root logger

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

