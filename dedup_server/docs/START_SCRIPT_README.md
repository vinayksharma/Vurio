# Start Script Documentation

## Overview

The `start.sh` script provides a convenient way to start the Media Deduplication Server with various configuration options.

## Usage

### Basic Usage

```bash
./start.sh
```

Starts the server with default settings.

### Available Options

| Option         | Description                   | Example                              |
| -------------- | ----------------------------- | ------------------------------------ |
| `-h, --help`   | Show help message             | `./start.sh --help`                  |
| `-b, --build`  | Build project before starting | `./start.sh --build`                 |
| `-d, --debug`  | Start in debug mode           | `./start.sh --debug`                 |
| `-c, --config` | Use specific config file      | `./start.sh --config my_config.yaml` |
| `-p, --port`   | Use specific port             | `./start.sh --port 9090`             |
| `--host`       | Use specific host             | `./start.sh --host localhost`        |

### Examples

#### Start with defaults

```bash
./start.sh
```

#### Build and start

```bash
./start.sh --build
```

#### Start in debug mode

```bash
./start.sh --debug
```

#### Start on different port

```bash
./start.sh --port 9090
```

#### Start on localhost only

```bash
./start.sh --host localhost
```

## Server Features

### Web API Endpoints

- `GET /api/v1/config` - Get all configuration
- `GET /api/v1/config/{key}` - Get specific property
- `PUT /api/v1/config/{key}` - Update property
- `POST /api/v1/config/reload` - Reload configuration
- `GET /api/v1/config/status` - Get system status
- `GET /api/openapi.json` - OpenAPI specification

### Console Commands

When the server is running, you can use these console commands:

- `help` - Show available commands
- `status` - Show server status
- `restart` - Restart web server
- `exit` - Stop the server

### Signal Handling

- `Ctrl+C` - Gracefully shutdown the server
- `SIGTERM` - Gracefully shutdown the server
- `SIGQUIT` - Gracefully shutdown the server

## Directory Structure

The script automatically creates necessary directories:

- `logs/` - Log files
- `data/` - Database files
- `config/` - Configuration files

## Default Configuration

- **Host**: 0.0.0.0 (all interfaces)
- **Port**: 8080
- **Config File**: config/config.yaml
- **Database**: data/dedup_server.db

## Troubleshooting

### Server executable not found

If you get an error about the server executable not being found, the script will automatically attempt to build the project first.

### Permission denied

Make sure the script is executable:

```bash
chmod +x start.sh
```

### Port already in use

If port 8080 is already in use, try a different port:

```bash
./start.sh --port 9090
```

## Integration with Console Input Manager

The start script works seamlessly with the new Console Input Manager, providing:

- Interactive console interface
- Thread-safe event handling
- Graceful shutdown capabilities
- Real-time command processing

The server will run continuously until explicitly stopped via console commands or signals.
