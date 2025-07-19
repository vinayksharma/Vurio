# Configuration Documentation

## Overview

The Dedup Server uses a JSON configuration file (`config.json`) to manage server settings. The server will automatically create a default configuration file if none exists.

## Configuration File Format

### Default Configuration

```json
{
  "auth_secret": "your-secret-key-here",
  "dedup_mode": "BALANCED",
  "log_level": "INFO",
  "server_host": "localhost",
  "server_port": 8080
}
```

## Configuration Fields

### `dedup_mode` (enum)

Deduplication processing mode that determines the balance between speed and accuracy.

**Valid Values:**

- `"FAST"` - Fast processing with basic deduplication
- `"BALANCED"` - Balanced processing with moderate accuracy (default)
- `"QUALITY"` - High-quality processing with maximum accuracy

### `log_level` (enum)

Logging verbosity level that controls the amount of log output.

**Valid Values:**

- `"TRACE"` - Most verbose logging (all messages)
- `"DEBUG"` - Debug information and above
- `"INFO"` - General information and above (default)
- `"WARN"` - Warning messages and above
- `"ERROR"` - Error messages only

### `server_port` (integer)

HTTP server port number for the REST API.

**Valid Range:** 1-65535

### `server_host` (string)

HTTP server host address for the REST API.

**Example:** `"localhost"`, `"0.0.0.0"`, `"127.0.0.1"`

### `auth_secret` (string)

JWT authentication secret key used for token generation and validation.

**Security Note:** Change this from the default value in production environments.

## Auto-Save Functionality

Configuration changes made through the API are automatically saved to `config.json`. This ensures that:

- Changes persist across server restarts
- No manual save step is required
- Configuration file always matches runtime settings

## API Endpoints

- `GET /config` - Retrieve current configuration
- `PUT /config` - Update configuration (auto-saves to file)
- `POST /config/reload` - Reload configuration from file
- `POST /config/save` - Manually save configuration to file

## Example Configuration Updates

### Update Dedup Mode

```bash
curl -X PUT http://localhost:8080/config \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer <token>" \
  -d '{"dedup_mode": "FAST"}'
```

### Update Log Level

```bash
curl -X PUT http://localhost:8080/config \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer <token>" \
  -d '{"log_level": "DEBUG"}'
```

### Update Server Port

```bash
curl -X PUT http://localhost:8080/config \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer <token>" \
  -d '{"server_port": 9090}'
```

## Validation

The server validates all configuration values:

- **Enum validation** for `dedup_mode` and `log_level`
- **Range validation** for `server_port` (1-65535)
- **Required field validation** for all configuration fields
- **Type validation** for all fields

Invalid configurations will be rejected with clear error messages.
