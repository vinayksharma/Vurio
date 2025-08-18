# Tests Directory

This directory contains all tests for the Dedup Server project, organized by type and category.

## Directory Structure

```
tests/
├── README.md                           # This file
├── CMakeLists.txt                      # Test build configuration
├── test_base.hpp                       # Common test utilities
├── scripts/                            # Test execution scripts
│   ├── run_all_tests.sh               # Master test runner
│   ├── run_unit_tests.sh              # Unit test runner
│   ├── run_integration_tests.sh       # Integration test runner
│   ├── test_quality_stack.sh          # Quality stack testing
│   ├── test_raw_support.sh            # RAW format support testing
│   ├── test_scan.sh                   # File scanning testing
│   ├── run_tests.sh                   # Legacy test runner
│   ├── clean_db.sh                    # Database cleanup utility
│   ├── clear_database.sh              # Basic database clearing
│   ├── clear_database_advanced.sh     # Advanced database management
│   ├── test_database_hash.sh          # Database hash functionality testing
│   └── test_hash_changes.sh           # Hash change behavior testing
├── integration/                        # Integration tests (standalone executables)
│   ├── cache_size_test.cpp            # Cache management tests
│   ├── smart_cache_cleanup_test.cpp   # Smart cache cleanup tests
│   ├── raw_file_test.cpp              # RAW file processing tests
│   ├── media_processor_example.cpp    # Media processor tests
│   ├── file_processor_example.cpp     # File processor tests
│   ├── file_type_config_test.cpp      # Configuration tests
│   ├── file_utils_example.cpp         # File utility tests
│   ├── debug_config.cpp               # Debug configuration tests
│   ├── test_arw_detection.cpp         # ARW format detection tests
│   ├── test_libraw_formats.cpp        # LibRaw format support tests
│   ├── test_libraw_raf.cpp            # RAF format tests
│   ├── test_mount_manager.cpp         # Mount manager tests
│   ├── test_mode_change.cpp           # Mode change testing
│   ├── test_mode_change               # Mode change executable
│   └── migrate_to_relative_paths.cpp # Path migration tests
├── libraw/                             # LibRaw-specific tests
│   ├── libraw_probe.cpp               # LibRaw probe tool
│   └── raw_to_jpeg.cpp                # RAW to JPEG conversion tests
├── test_data/                          # Test media files and data
│   ├── audio/                         # Audio test files
│   │   └── test_audio.mp3            # Test MP3 file
│   └── images/                        # Image test files
│       └── test_balance.jpg           # Test JPG file
└── [unit test files]                   # Google Test-based unit tests
    ├── database_manager_test.cpp
    ├── file_processor_test.cpp
    ├── media_processor_test.cpp
    ├── media_processing_orchestrator_test.cpp
    ├── file_utils_test.cpp
    ├── auth_test.cpp
    └── status_test.cpp
```

## Test Categories

### 1. Unit Tests

- **Framework**: Google Test (gtest)
- **Purpose**: Test individual components in isolation
- **Execution**: `./tests/scripts/run_unit_tests.sh`
- **Output**: Standard gtest output with colored results

### 2. Integration Tests

- **Framework**: Standalone C++ executables
- **Purpose**: Test component interactions and end-to-end workflows
- **Execution**: `./tests/scripts/run_integration_tests.sh`
- **Output**: Comprehensive JSON report with timing and categorization

### 3. Script Tests

- **Framework**: Bash scripts
- **Purpose**: Test system-level functionality and workflows
- **Execution**: Automatically run by integration test harness
- **Output**: Included in integration test results

#### Database Hash Tests

- **test_database_hash.sh**: Comprehensive testing of database hash functionality
  - Tests database and table hash generation
  - Validates error handling for non-existent tables
  - Demonstrates hash changes when data is modified
- **test_hash_changes.sh**: Tests hash behavior with data changes
  - Verifies hash changes only when content changes
  - Confirms hash stability during internal database operations
  - Demonstrates deterministic hash generation

### Test Data

- **Location**: `tests/test_data/`
- **Audio Files**: `tests/test_data/audio/` - Contains test MP3 files for audio processing tests
- **Image Files**: `tests/test_data/images/` - Contains test JPG files for image processing tests
- **Purpose**: Provides consistent test media files for reproducible testing
- **Usage**: Tests can reference these files using relative paths from the test data directory

## Test Execution

### Quick Start

```bash
# Run all tests (unit + integration)
./tests/scripts/run_all_tests.sh

# Run only unit tests
./tests/scripts/run_all_tests.sh --unit

# Run only integration tests
./tests/scripts/run_all_tests.sh --integration
```

### Individual Test Runners

```bash
# Unit tests only
./tests/scripts/run_unit_tests.sh

# Integration tests only
./tests/scripts/run_integration_tests.sh

# Integration tests with options
./tests/scripts/run_integration_tests.sh --help
./tests/scripts/run_integration_tests.sh --category cache
./tests/scripts/run_integration_tests.sh --test cache_size_test
```

## Test Categories

### Cache Management

- **cache_size_test**: Tests cache size limits and management
- **smart_cache_cleanup_test**: Tests intelligent cache cleanup strategies

### File Processing

- **raw_file_test**: Tests RAW file handling and processing
- **media_processor_example**: Tests media processing workflows
- **file_processor_example**: Tests file processing pipelines

### LibRaw Support

- **libraw_probe**: Tests LibRaw format detection
- **raw_to_jpeg**: Tests RAW to JPEG conversion
- **test_libraw_formats**: Tests various RAW format support
- **test_libraw_raf**: Tests RAF format specifically

### Configuration & Utilities

- **file_type_config_test**: Tests file type configuration
- **file_utils_example**: Tests file utility functions
- **debug_config**: Tests debug configuration options

## Build Configuration

All tests are configured in `tests/CMakeLists.txt` and automatically included in the main build. The main `CMakeLists.txt` simply includes the tests directory:

```cmake
add_subdirectory(tests)
```

## Test Results

### Unit Tests

- Output: Standard gtest output to console
- Exit code: 0 for success, 1 for failure

### Integration Tests

- **Console Output**: Colored progress and summary
- **Log File**: `tests/integration_tests.log`
- **JSON Report**: `tests/test_results.json`
- **Exit Code**: 0 for success, 1 for failure

### Sample JSON Report

```json
{
  "timestamp": "2024-01-15T10:30:00Z",
  "summary": {
    "total_tests": 15,
    "passed": 14,
    "failed": 1,
    "skipped": 0,
    "success_rate": "93.3%"
  },
  "categories": {
    "cache": {
      "description": "Cache management and cleanup tests",
      "total": 2,
      "passed": 2,
      "failed": 0,
      "skipped": 0
    }
  },
  "tests": {
    "cache_size_test": {
      "category": "cache",
      "result": "PASSED",
      "duration": 1.234
    }
  }
}
```

## Adding New Tests

### Unit Tests

1. Create test file in `tests/` directory
2. Add to `tests/CMakeLists.txt` in the `dedup_tests` executable
3. Use Google Test framework

### Integration Tests

1. Create test file in appropriate subdirectory:
   - `tests/integration/` for general integration tests
   - `tests/libraw/` for LibRaw-specific tests
2. Add to `tests/CMakeLists.txt` with proper dependencies
3. Update test definitions in `run_integration_tests.sh`

### Script Tests

1. Create script in `tests/scripts/` directory
2. Update `run_script_tests()` function in `run_integration_tests.sh`

## Dependencies

### Required Tools

- **bc**: For floating-point arithmetic in test timing
- **bash**: For test execution scripts
- **CMake**: For building tests
- **Google Test**: For unit testing framework

### Build Dependencies

All test dependencies are automatically handled by CMake and linked appropriately for each test type.

## Troubleshooting

### Common Issues

1. **Test binary not found**

   - Ensure you've run `rebuild.sh` or `build.sh`
   - Check that CMake configuration is correct

2. **Permission denied**

   - Make sure test scripts are executable: `chmod +x tests/scripts/*.sh`

3. **Tests failing**
   - Check the log files for detailed error information
   - Verify that all dependencies are properly installed
   - Check that the build is up to date

### Debug Mode

```bash
# Enable verbose output
./tests/scripts/run_all_tests.sh --verbose

# Run specific test with verbose output
./tests/scripts/run_integration_tests.sh --verbose --test cache_size_test
```

## Performance Considerations

- **Unit Tests**: Fast execution, run frequently during development
- **Integration Tests**: Slower execution, run before commits and releases
- **Test Isolation**: Each test runs independently to avoid interference
- **Resource Cleanup**: Tests automatically clean up after themselves

## Continuous Integration

The test suite is designed to work in CI/CD environments:

- Exit codes properly indicate success/failure
- JSON output can be parsed by CI systems
- Tests can be run individually or in categories
- Comprehensive logging for debugging CI issues
