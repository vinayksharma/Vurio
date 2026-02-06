# Test Scripts

This directory contains various test scripts for the Media Deduplication Server.

## Scripts

### `run_tests.sh`
Main test runner script that provides comprehensive testing capabilities:
- Unit tests
- Integration tests  
- Performance tests
- Code coverage
- Memory checking
- Parallel test execution

**Usage:**
```bash
cd tests/scripts
./run_tests.sh [options]
```

### `demo_web_server.sh`
Demonstrates the web server functionality by:
- Starting the server
- Testing all API endpoints
- Showing configuration management
- Graceful shutdown

**Usage:**
```bash
cd tests/scripts
./demo_web_server.sh [--start|--test]
```

### `test_web_server.sh`
Simple web server API test script that:
- Checks server status
- Tests basic API endpoints
- Validates configuration endpoints

**Usage:**
```bash
cd tests/scripts
./test_web_server.sh [--server|--test]
```

### `test_webserver_restart.sh`
Tests the web server restart functionality:
- Changes server configuration
- Restarts web server with new settings
- Verifies restart worked correctly
- Tests port changes

**Usage:**
```bash
cd tests/scripts
./test_webserver_restart.sh
```

## Running Scripts

All scripts are designed to be run from the `tests/scripts` directory. They automatically change to the project root directory when executed.

## Prerequisites

- Project must be built (`./build.sh` from project root)
- Server dependencies must be installed
- Database directory must exist (`mkdir -p data`)

## Notes

- Scripts use relative paths and will automatically navigate to the project root
- All scripts include proper error handling and colored output
- Scripts can be interrupted with Ctrl+C and will clean up properly
