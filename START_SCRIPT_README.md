# Start Script Documentation

## Overview

The `start.sh` script provides a simple and convenient way to start the Media Deduplication Server with the web API.

## Usage

### Basic Usage

```bash
# Start the server with default settings
./start.sh

# Show help information
./start.sh --help
```

### Options

| Option         | Description                          | Example                       |
| -------------- | ------------------------------------ | ----------------------------- |
| `-h, --help`   | Show help message                    | `./start.sh --help`           |
| `-b, --build`  | Build the project before starting    | `./start.sh --build`          |
| `-d, --debug`  | Start in debug mode                  | `./start.sh --debug`          |
| `-c, --config` | Use specific config file             | `./start.sh --config my.yaml` |
| `-p, --port`   | Use specific port (default: 8080)    | `./start.sh --port 9090`      |
| `--host`       | Use specific host (default: 0.0.0.0) | `./start.sh --host localhost` |

### Examples

```bash
# Start server with defaults
./start.sh

# Build and start server
./start.sh --build

# Start in debug mode
./start.sh --debug

# Start on different port
./start.sh --port 9090

# Start on localhost only
./start.sh --host localhost

# Use custom config file
./start.sh --config config/production.yaml

# Combine options
./start.sh --build --debug --port 9090
```

## What the Script Does

1. **Validates Environment**: Checks if you're in the correct directory and if required files exist
2. **Builds Project** (if requested): Runs `build.sh` to compile the project
3. **Creates Directories**: Ensures necessary directories (`logs`, `data`, `config`) exist
4. **Starts Server**: Launches the Media Deduplication Server with web API
5. **Shows Information**: Displays server status and available API endpoints

## Server Information

When the server starts, you'll see:

```
==========================================
  Media Deduplication Server
==========================================

[INFO] Starting Media Deduplication Server...
[INFO] Web API will be available at: http://localhost:8080
[INFO] API endpoints:
[INFO]   GET  /api/v1/config - Get all configuration
[INFO]   GET  /api/v1/config/{key} - Get specific property
[INFO]   PUT  /api/v1/config/{key} - Update property
[INFO]   POST /api/v1/config/reload - Reload configuration
[INFO]   GET  /api/v1/config/status - Get system status
[INFO]   GET  /api/openapi.json - OpenAPI specification
[INFO] Press Ctrl+C to stop the server
```

## API Endpoints

Once the server is running, you can access:

- **Configuration Management**: `http://localhost:8080/api/v1/config`
- **System Status**: `http://localhost:8080/api/v1/config/status`
- **OpenAPI Documentation**: `http://localhost:8080/api/openapi.json`

## Stopping the Server

Press `Ctrl+C` to stop the server gracefully.

## Troubleshooting

### Server Won't Start

- Make sure you're in the project root directory
- Ensure the project has been built (`./build.sh`)
- Check if port 8080 is available

### Build Issues

- Run `./start.sh --build` to build before starting
- Check that all dependencies are installed
- Verify CMake and build tools are available

### Configuration Issues

- Use `--config` option to specify a custom config file
- Check that the config file exists and is valid
- Default config is created automatically if missing

## Integration with Other Scripts

The `start.sh` script works alongside other project scripts:

- `build.sh` - Builds the project
- `run.sh` - More comprehensive run script with additional options
- `demo_web_server.sh` - Demonstrates the web API functionality
- `rebuild.sh` - Clean build and test cycle

## Development Workflow

For development, you might use:

```bash
# Quick start during development
./start.sh --build --debug

# Production-like start
./start.sh --config config/production.yaml

# Test with demo
./demo_web_server.sh
```

This script provides a simple entry point for starting the Media Deduplication Server with all its web API capabilities.
