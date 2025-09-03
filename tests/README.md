# Testing Framework for Media Deduplication Server

This directory contains a comprehensive testing framework for the Media Deduplication Server, including unit tests, integration tests, and performance tests.

## 🏗️ **Test Structure**

```
tests/
├── CMakeLists.txt              # Test build configuration
├── run_tests.sh                # Test runner script
├── test_utils.hpp              # Test utilities header
├── test_utils.cpp              # Test utilities implementation
├── README.md                   # This file
├── unit/                       # Unit tests
│   ├── test_observable_property.cpp
│   ├── test_log_level.cpp
│   ├── test_observable_log_level.cpp
│   ├── test_observable_config_manager.cpp
│   ├── test_config_manager.cpp
│   ├── test_database_manager.cpp
│   └── test_server.cpp
├── integration/                # Integration tests
│   ├── test_config_file_sync.cpp
│   ├── test_property_change_callbacks.cpp
│   ├── test_file_monitoring.cpp
│   └── test_config_validation.cpp
├── performance/                # Performance tests
│   ├── test_property_access_performance.cpp
│   ├── test_file_monitoring_performance.cpp
│   └── test_callback_performance.cpp
└── mocks/                      # Mock objects
    ├── mock_file_system.cpp
    ├── mock_logger.cpp
    └── mock_config_storage.cpp
```

## 🧪 **Test Types**

### **Unit Tests** (`unit/`)

- **Purpose**: Test individual components in isolation
- **Scope**: Single class or function
- **Dependencies**: Minimal, often mocked
- **Speed**: Fast execution
- **Examples**: ObservableProperty, LogLevel, individual methods

### **Integration Tests** (`integration/`)

- **Purpose**: Test component interactions and workflows
- **Scope**: Multiple components working together
- **Dependencies**: Real file system, configuration files
- **Speed**: Medium execution time
- **Examples**: Configuration file synchronization, property change callbacks

### **Performance Tests** (`performance/`)

- **Purpose**: Measure and validate performance characteristics
- **Scope**: System performance under load
- **Dependencies**: Real components, performance metrics
- **Speed**: Longer execution time
- **Examples**: Property access performance, concurrent operations

## 🚀 **Quick Start**

### **1. Build the Project**

```bash
# From project root
./build.sh

# Or manually
mkdir -p build
cd build
cmake ..
make
```

### **2. Run All Tests**

```bash
# From project root
./run.sh test

# Or from tests directory
cd tests
./run_tests.sh
```

### **3. Run Specific Test Types**

```bash
# Unit tests only
./run_tests.sh unit

# Integration tests only
./run_tests.sh integration

# Performance tests only
./run_tests.sh performance
```

### **4. Run Specific Tests**

```bash
# Run a specific test
./run_tests.sh specific test_observable_property

# Run tests matching a pattern
./run_tests.sh --filter "LogLevel"
```

## 🔧 **Test Runner Options**

### **Basic Options**

```bash
./run_tests.sh [OPTIONS] [TEST_TYPE]

OPTIONS:
    -h, --help          Show help message
    -v, --verbose       Enable verbose output
    -d, --debug         Enable debug mode
    -c, --coverage      Enable coverage reporting
    -j, --jobs N        Number of parallel jobs
    --output-dir DIR    Test output directory
    --filter PATTERN    Filter tests by pattern
    --repeat N          Repeat tests N times
    --timeout SEC       Test timeout in seconds
    --memory-check      Enable memory checking
```

### **Advanced Usage Examples**

```bash
# Run with coverage and verbose output
./run_tests.sh --coverage --verbose

# Run tests in parallel with 4 jobs
./run_tests.sh --jobs 4

# Run tests 3 times to check for flakiness
./run_tests.sh --repeat 3

# Run with memory checking (valgrind/asan)
./run_tests.sh --memory-check

# Run specific test with debug output
./run_tests.sh --debug --verbose specific test_log_level
```

## 🛠️ **Test Utilities**

### **TestUtils Class**

The `TestUtils` class provides common testing functionality:

```cpp
#include "test_utils.hpp"

// Random data generation
std::string random_str = TestUtils::generateRandomString(10);
int random_int = TestUtils::generateRandomInt(1, 100);
LogLevel random_level = TestUtils::generateRandomLogLevel();

// File operations
std::string temp_file = TestUtils::generateTempFilePath("test", "yaml");
TestUtils::createTempFile("content", temp_file);
std::string content = TestUtils::readFileContent(temp_file);

// Timing and synchronization
auto start = TestUtils::getCurrentTime();
// ... do work ...
auto duration = TestUtils::getElapsedTime(start);

// Wait for conditions
bool success = TestUtils::waitForCondition(
    []() { return someCondition(); },
    std::chrono::seconds(10)
);
```

### **Test Fixtures**

Pre-built test fixtures for common scenarios:

```cpp
// Basic fixture with environment setup
class MyTest : public TestFixture {
    void SetUp() override {
        TestFixture::SetUp();
        // Additional setup
    }

    void TearDown() override {
        // Additional cleanup
        TestFixture::TearDown();
    }
};

// Configuration-specific fixture
class ConfigTest : public ConfigTestFixture {
    // Provides temp_config_file_ and temp_config_dir_
    // Automatically creates and cleans up test configs
};

// Performance testing fixture
class PerfTest : public PerformanceTestFixture {
    // Provides timing utilities and performance metrics
};
```

### **Custom Assertions**

Extended assertion functions for specific test scenarios:

```cpp
#include "test_utils.hpp"

// File assertions
Assert::assertFileExists("config.yaml");
Assert::assertFileContent("config.yaml", "expected content");
Assert::assertFileContains("config.yaml", "key: value");

// Performance assertions
Assert::assertPerformance(actual_time, max_time);
Assert::assertWithinRange(value, min, max);

// String and container assertions
Assert::assertStringContains(haystack, needle);
Assert::assertVectorSize(vector, expected_size);
```

## 📊 **Test Data Generation**

### **Configuration Test Data**

```cpp
// Generate valid logging configuration
auto valid_config = TestData::getValidLoggingConfig();

// Generate invalid configuration for testing
auto invalid_config = TestData::getInvalidLoggingConfig();

// Generate performance test configuration
auto perf_config = TestData::getPerformanceTestConfig();
```

### **File Test Data**

```cpp
// Generate large text content
std::string large_content = TestData::getLargeTextContent(1024); // 1MB

// Generate binary content
std::string binary_content = TestData::getBinaryContent(1000); // 1KB

// Get test file extensions
auto extensions = TestData::getTestFileExtensions();
```

## 🔍 **Test Execution Details**

### **CMake Integration**

Tests are automatically integrated with CMake:

```cmake
# Enable testing
enable_testing()

# Find Google Test
find_package(GTest REQUIRED)

# Create test executables
foreach(test_source ${UNIT_TEST_SOURCES})
    get_filename_component(test_name ${test_source} NAME_WE)
    add_executable(${test_name} ${test_source})
    target_link_libraries(${test_name} ${GTEST_LIBRARIES} ${Poco_LIBRARIES})
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
```

### **Test Labels and Properties**

Tests are categorized with labels for easy filtering:

```cmake
set_tests_properties(${UNIT_TEST_SOURCES} PROPERTIES
    LABELS "unit"
    ENVIRONMENT "TEST_TYPE=unit"
)

set_tests_properties(${INTEGRATION_TEST_SOURCES} PROPERTIES
    LABELS "integration"
    ENVIRONMENT "TEST_TYPE=integration"
)
```

### **Test Output and Logging**

- Test results are saved to `build/test_results/`
- Each test generates a log file
- Coverage reports go to `build/coverage/`
- Failed tests show detailed output

## 🧹 **Test Maintenance**

### **Adding New Tests**

1. **Create test file** in appropriate directory (`unit/`, `integration/`, `performance/`)
2. **Include test utilities**: `#include "test_utils.hpp"`
3. **Use appropriate fixture** or inherit from `TestFixture`
4. **Add to CMakeLists.txt** in the appropriate test source list
5. **Follow naming convention**: `test_<component_name>.cpp`

### **Test Naming Conventions**

- **Test files**: `test_<component_name>.cpp`
- **Test classes**: `<ComponentName>Test`
- **Test methods**: `TestName_Scenario_ExpectedResult`
- **Example**: `ObservablePropertyTest_SetValue_TriggersCallback`

### **Best Practices**

- **One assertion per test** when possible
- **Use descriptive test names** that explain the scenario
- **Clean up resources** in `TearDown()`
- **Test both success and failure cases**
- **Use appropriate test fixtures** for common setup
- **Include performance benchmarks** for critical paths

## 🐛 **Debugging Tests**

### **Enable Debug Output**

```bash
# Run with debug mode
./run_tests.sh --debug

# Run with verbose output
./run_tests.sh --verbose

# Run specific test with debug
./run_tests.sh --debug --verbose specific test_observable_property
```

### **Debug Environment Variables**

```bash
# Enable Google Test debug output
export GTEST_DEBUG=1

# Enable Poco logging
export POCO_LOG_LEVEL=debug

# Enable memory checking
export ASAN_OPTIONS="detect_leaks=1:abort_on_error=1"
```

### **Common Debug Scenarios**

- **Test failures**: Check test output and logs
- **Memory leaks**: Use `--memory-check` option
- **Performance issues**: Run performance tests with `--verbose`
- **Flaky tests**: Use `--repeat` to identify intermittent failures

## 📈 **Performance Testing**

### **Performance Thresholds**

Tests include performance thresholds that can be adjusted:

```cpp
// Performance thresholds (adjust based on your system)
const std::chrono::microseconds GET_VALUE_THRESHOLD{100};      // 100 microseconds
const std::chrono::microseconds SET_VALUE_THRESHOLD{200};      // 200 microseconds
const std::chrono::microseconds CALLBACK_THRESHOLD{50};        // 50 microseconds
```

### **Performance Metrics**

Tests automatically report:

- **Total execution time**
- **Operations per second**
- **Average time per operation**
- **Concurrent operation performance**

### **Benchmarking**

```bash
# Run performance tests
./run_tests.sh performance

# Run with specific iterations
./run_tests.sh --filter "Performance" --verbose

# Compare performance across runs
./run_tests.sh --repeat 5 performance
```

## 🔒 **Test Security and Isolation**

### **Temporary File Management**

- Tests use temporary files and directories
- Automatic cleanup in `TearDown()`
- Unique naming to avoid conflicts
- Isolated from production data

### **Environment Isolation**

- Tests set `TEST_MODE=true`
- Separate test data directories
- Environment variable cleanup
- No interference with system configuration

### **Resource Management**

- Automatic cleanup of test resources
- Memory leak detection with `--memory-check`
- File descriptor cleanup
- Database connection isolation

## 📚 **Additional Resources**

### **Google Test Documentation**

- [Google Test Primer](https://github.com/google/googletest/blob/main/googletest/docs/primer.md)
- [Advanced Google Test](https://github.com/google/googletest/blob/main/googletest/docs/advanced.md)
- [Google Mock](https://github.com/google/googletest/blob/main/googlemock/docs/for_dummies.md)

### **CMake Testing**

- [CMake Testing](https://cmake.org/cmake/help/latest/command/enable_testing.html)
- [CTest](https://cmake.org/cmake/help/latest/manual/ctest.1.html)

### **Performance Testing**

- [Performance Testing Best Practices](https://en.wikipedia.org/wiki/Software_performance_testing)
- [Benchmarking Guidelines](https://github.com/google/benchmark)

---

**This testing framework provides a solid foundation for ensuring the quality and reliability of your Media Deduplication Server! 🎯**
