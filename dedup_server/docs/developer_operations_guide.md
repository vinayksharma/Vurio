# Build Tasks and IDE Configuration

This document describes the available build tasks and IDE configuration for the Media Deduplication Server project.

## 🚀 Quick Start (VS Code / Cursor)

### Default Build Task (Cmd+Shift+B / Ctrl+Shift+B)

Press **Cmd+Shift+B** (macOS) or **Ctrl+Shift+B** (Linux/Windows) to run the **fast rebuild with all tests**.

This runs `./scripts/smart_rebuild` which:
- ✅ Cleans all build artifacts
- ✅ **Preserves dependency cache** (no re-download of ONNX Runtime)
- ✅ Full project rebuild
- ✅ Runs all unit tests
- ✅ Runs integration tests

**Time:** ~30-60 seconds (depends on changes)

## 📋 Available Build Tasks

All tasks can be accessed via:
- **Cmd+Shift+P** (macOS) or **Ctrl+Shift+P** (Linux/Windows)
- Type: "Tasks: Run Task"
- Select from the list below

### 1. Rebuild and Test All (Fast) ⭐ DEFAULT

**Command:** `./scripts/smart_rebuild`

**What it does:**
- Complete clean of build artifacts
- Preserves `build/_deps/` cache (ONNX Runtime stays cached)
- Full rebuild from scratch
- Runs all unit tests
- Runs integration tests via ctest

**When to use:**
- Regular development workflow
- Before committing changes
- After pulling updates
- Testing comprehensive changes

**Time:** ~30-60 seconds

---

### 2. Full Rebuild (Clean Everything)

**Command:** `./scripts/rebuild`

**What it does:**
- **Complete clean** including all dependencies
- **Re-downloads** ONNX Runtime (~200MB)
- Full rebuild from scratch
- Runs all unit tests
- Runs integration tests via ctest

**When to use:**
- Suspected dependency corruption
- Major CMake configuration changes
- Clean slate verification
- Troubleshooting build issues

**Time:** ~3-5 minutes (includes download)

---

### 3. Clean Build Server

**Command:** `./build.sh --clean`

**What it does:**
- Cleans build artifacts (preserves `_deps/`)
- Full rebuild
- **Does NOT run tests**

**When to use:**
- Quick rebuild without testing
- Just want to verify compilation
- Testing build system changes

**Time:** ~20-40 seconds

---

### 4. Build Server (No Clean)

**Command:** `./build.sh`

**What it does:**
- Incremental build (only changed files)
- **Does NOT run tests**

**When to use:**
- Rapid iteration during development
- Small changes to single files
- Quick compilation check

**Time:** ~5-15 seconds

---

### 5. Debug Build

**Command:** `./build.sh --clean --debug`

**What it does:**
- Cleans build artifacts
- Builds with debug symbols (`-g -O0`)
- Enables all warnings
- **Does NOT run tests**

**When to use:**
- Preparing for debugging session
- Need debug symbols for GDB/LLDB
- Investigating crashes or issues

**Time:** ~30-50 seconds

---

### 6. Build and Test

**Command:** `./build.sh --clean --test`

**What it does:**
- Cleans build artifacts (preserves `_deps/`)
- Full rebuild
- Runs unit tests only (not integration tests)

**When to use:**
- Quick test validation
- Don't need ctest integration tests
- Faster than smart_rebuild

**Time:** ~25-45 seconds

---

## 🎯 Recommended Workflow

### Daily Development
```bash
# Make changes to code
# Press Cmd+Shift+B (runs smart_rebuild)
# All tests pass → commit
```

### After Git Pull
```bash
# git pull origin main
# Press Cmd+Shift+B (runs smart_rebuild)
# Verify everything builds and tests pass
```

### Troubleshooting Build Issues
```bash
# Cmd+Shift+P → "Full Rebuild (Clean Everything)"
# Re-downloads all dependencies
# Clean slate build
```

### Quick Iteration (No Tests)
```bash
# Cmd+Shift+P → "Build Server (No Clean)"
# Fast incremental build
# Manually run: build/bin/media_dedup_server
```

---

## 🔧 Manual Build Scripts

You can also run build scripts directly from the terminal:

```bash
# Smart rebuild (fast, preserves cache)
./scripts/smart_rebuild

# Full rebuild (clean everything)
./scripts/rebuild

# Standard build
./build.sh

# Build with options
./build.sh --clean          # Clean build
./build.sh --debug          # Debug build
./build.sh --test           # Build and test
./build.sh --clean --test   # Clean, build, and test
```

---

## 📊 Build Task Comparison

| Task | Clean | Cache | Tests | Time | Use Case |
|------|-------|-------|-------|------|----------|
| **Smart Rebuild** ⭐ | ✅ | ✅ Keep | ✅ All | ~45s | **Default development** |
| **Full Rebuild** | ✅ | ❌ Delete | ✅ All | ~4m | Troubleshooting |
| **Clean Build** | ✅ | ✅ Keep | ❌ | ~30s | Quick compile check |
| **Incremental** | ❌ | ✅ Keep | ❌ | ~10s | Rapid iteration |
| **Debug Build** | ✅ | ✅ Keep | ❌ | ~40s | Debugging session |
| **Build & Test** | ✅ | ✅ Keep | ⚠️ Unit | ~35s | Quick validation |

---

## 🛠️ IDE Configuration Files

The following VS Code/Cursor configuration files are tracked in the repository:

### `.vscode/tasks.json`
Defines all build tasks and keyboard shortcuts.

### `.vscode/launch.json`
Debugger configuration for F5 debugging.

### `.vscode/settings.json`
Project-specific editor settings (formatting, linting, etc.).

### `.vscode/keybindings.json`
Custom keyboard shortcuts for build and debug operations.

---

## 🔑 Keyboard Shortcuts (VS Code / Cursor)

| Shortcut | Task | Description |
|----------|------|-------------|
| **Cmd+Shift+B** | Smart Rebuild | Fast rebuild with tests (default) |
| **F5** | Debug Build & Run | Build in debug mode and attach debugger |
| **Shift+F5** | Stop Debugger | Stop debugging session |
| **Cmd+Shift+P** | Command Palette | Access all tasks |

---

## 🐛 Debugging

### Quick Debug (F5)

1. Set breakpoints in your code
2. Press **F5**
3. Builds in debug mode automatically
4. Attaches debugger to the server
5. Server starts and breaks at your breakpoints

See `.vscode/launch.json` for debugger configuration.

---

## 📦 Dependency Cache

### What is cached?
- ONNX Runtime headers (~200MB tarball)
- External dependencies from CMake FetchContent
- Located in `build/_deps/`

### When is cache cleared?
- **Full Rebuild** task (deliberate)
- Manual `rm -rf build`
- CMake dependency version changes

### When is cache preserved?
- **Smart Rebuild** task ✅
- **Clean Build** task ✅
- All other build tasks ✅

---

## 💡 Tips

1. **Use Smart Rebuild for daily work** - It's fast and thorough
2. **Run Full Rebuild weekly** - Ensures clean dependency state
3. **Use Incremental builds** - For rapid testing during development
4. **Always test before committing** - Cmd+Shift+B runs all tests
5. **Debug with F5** - Automatic debug build and attach

---

## 🚨 Common Issues

### "Tests failed after rebuild"
- Check test output for specific failures
- Try "Full Rebuild (Clean Everything)" to rule out cache issues
- Verify database and config files are not corrupted

### "Build takes forever"
- Check if you're running Full Rebuild (re-downloads dependencies)
- Use Smart Rebuild instead for faster builds
- Verify network connection if downloads are slow

### "CMake configuration errors"
- Run Full Rebuild to refresh CMake cache
- Check that all dependencies are installed
- Review CMake error messages in the terminal

### "Can't find build tasks"
- Ensure you're in the project root directory
- Reload VS Code/Cursor window
- Check that `.vscode/tasks.json` exists

---

## 📚 Related Documentation

- [Build Scripts](../scripts/README.md) - Manual build script documentation
- [Testing Guide](tests_README.md) - Comprehensive testing documentation
- [Configuration Reference](CONFIGURATION_REFERENCE.md) - Server configuration
- [Development Setup](../README.md) - Initial setup and dependencies

---

**Last Updated:** October 2025

---

# Promoting Your Media Deduplication Server Project

This guide covers strategies to help people discover and use your project.

## 🎯 Quick Wins (Do These First!)

### 1. Optimize GitHub Repository Metadata

**Add a compelling description:**

```bash
gh repo edit vinayksharma/dedup_server \
  --description "High-performance C++ media deduplication server with intelligent duplicate detection, REST API, and multi-format support"
```

**Add relevant topics/tags:**

```bash
gh repo edit vinayksharma/dedup_server \
  --add-topic "cplusplus" \
  --add-topic "deduplication" \
  --add-topic "media-processing" \
  --add-topic "sqlite" \
  --add-topic "image-processing" \
  --add-topic "duplicate-detection" \
  --add-topic "rest-api" \
  --add-topic "yaml-config" \
  --add-topic "media-management" \
  --add-topic "cmake" \
  --add-topic "poco-library"
```

**Add a homepage URL (if you have one):**

```bash
gh repo edit vinayksharma/dedup_server --homepage "https://github.com/vinayksharma/dedup_server#readme"
```

### 2. Enhance Your README.md

Your README is already good! Consider adding:

- **Badges** at the top (license, build status, version)
- **Screenshots/Demo** section
- **Quick Start** section (already have this)
- **Contributing** guidelines
- **Roadmap** section

### 3. Create a Release

```bash
# Tag the current version
git tag -a v1.0.0 -m "Initial public release"

# Push the tag
git push origin v1.0.0

# Create a GitHub release
gh release create v1.0.0 \
  --title "v1.0.0 - Initial Public Release" \
  --notes "First public release of Media Deduplication Server. Features include:
- Multi-format media support (images, videos, audio)
- Intelligent duplicate detection using SHA-256
- RESTful HTTP API with 13 endpoints
- Advanced scheduling and thread pool management
- Comprehensive documentation"
```

## 📢 Community Platforms

### Reddit Communities

**Technical/Programming:**

- `/r/cpp` - C++ community
- `/r/programming` - General programming
- `/r/opensource` - Open source projects
- `/r/selfhosted` - Self-hosted solutions
- `/r/DataHoarder` - People who manage large media collections
- `/r/photography` - Photographers who might need deduplication

**Post Format:**

```
Title: [Project] Media Deduplication Server - C++ server for finding duplicate media files

Body:
- Brief description
- Key features
- Why you built it
- GitHub link
- Ask for feedback
```

### Hacker News

Post to: https://news.ycombinator.com/show

**Title Format:**
"Show HN: Media Deduplication Server - C++ server for finding duplicate files"

**Tips:**

- Post during peak hours (9-11 AM PST)
- Be ready to answer questions
- Don't over-promote
- Focus on technical details

### Dev.to / Hashnode / Medium

Write a technical blog post:

- **Title**: "Building a High-Performance Media Deduplication Server in C++"
- **Content Ideas**:
  - Why you built it
  - Architecture decisions
  - Challenges faced
  - Performance benchmarks
  - Lessons learned
  - Link to GitHub

### Twitter/X

**Tweet Ideas:**

1. "Just open-sourced my Media Deduplication Server! Built in C++, handles images/videos/audio, uses SHA-256 hashing. Check it out: [link]"
2. "Spent 6 months building a deduplication server. Here's what I learned: [thread]"
3. Share interesting technical details or performance metrics

**Use hashtags:**
`#cpp #opensource #datamanagement #programming #softwaredevelopment`

### LinkedIn

Post about your project:

- Share your journey
- Technical challenges
- What you learned
- Ask for feedback

## 🎓 Technical Communities

### Stack Overflow

Answer questions related to:

- Media deduplication
- C++ performance
- Image processing
- When relevant, mention your project

### GitHub Discussions

Enable GitHub Discussions in your repo:

```bash
gh repo edit vinayksharma/dedup_server --enable-discussions
```

Then create discussion topics:

- "Use Cases and Examples"
- "Feature Requests"
- "Performance Tips"
- "Q&A"

### Discord/Slack Communities

**C++ Communities:**

- C++ Discord servers
- CppCon Slack
- Local programming meetups

**Photography/Media Communities:**

- Photography forums
- Media management tool communities

## 📝 Documentation & Content

### 1. Create a Project Website/Blog

Options:

- **GitHub Pages** (free, easy)
- **Netlify** (free tier)
- **Vercel** (free tier)

**Content Ideas:**

- Getting started guide
- API documentation
- Architecture overview
- Performance benchmarks
- Use case examples

### 2. Create Video Content

**YouTube:**

- Demo video (5-10 min)
- Architecture walkthrough
- Installation tutorial
- Use case demonstration

**Platforms:**

- YouTube
- TikTok/Instagram (short demos)
- Dev.to (embed videos)

### 3. Write Technical Articles

**Platforms:**

- Dev.to
- Medium
- Hashnode
- Your own blog

**Article Ideas:**

- "How I Built a Media Deduplication Server"
- "Performance Optimization in C++"
- "Designing a RESTful API for Media Management"
- "Lessons Learned from Building a Deduplication System"

## 🤝 Collaboration & Contribution

### Make It Easy to Contribute

1. **Add CONTRIBUTING.md:**

   - How to submit issues
   - Code style guidelines
   - How to submit PRs
   - Development setup

2. **Add CODE_OF_CONDUCT.md:**

   - Create a welcoming environment

3. **Label Issues:**

   - `good first issue`
   - `help wanted`
   - `documentation`
   - `bug`
   - `enhancement`

4. **Create Starter Issues:**
   - Small, well-defined tasks
   - Good for new contributors

### Engage with Similar Projects

- Star similar projects
- Comment on related issues
- Share your solution when relevant
- Contribute to related projects

## 📊 Metrics & Analytics

### Track Your Success

**GitHub Metrics:**

- Stars
- Forks
- Issues
- Pull Requests
- Contributors

**Tools:**

- GitHub Insights (built-in)
- Star History (https://star-history.com)
- Google Analytics (if you have a website)

## 🎯 Targeted Outreach

### 1. Find Similar Projects

Search GitHub for:

- "media deduplication"
- "duplicate finder"
- "image deduplication"
- "video deduplication"

**Engage by:**

- Opening issues to share your project
- Commenting on related discussions
- Creating comparisons

### 2. Reach Out to Bloggers/Influencers

Find tech bloggers who write about:

- C++ development
- Media management
- Open source projects
- Self-hosted solutions

**Politely ask:**

- To review your project
- To share it with their audience
- For feedback

### 3. Submit to Directories

**Open Source Directories:**

- **Awesome Lists**: Find relevant awesome lists and submit
- **GitHub Awesome**: Create an awesome list entry
- **AlternativeTo**: If there are similar commercial tools
- **Product Hunt**: Launch as a "maker" product

## 🔄 Ongoing Promotion

### Regular Activities

1. **Release Updates**

   - Create releases for major versions
   - Write detailed release notes
   - Share on social media

2. **Engage with Users**

   - Respond to issues promptly
   - Help with setup problems
   - Answer questions

3. **Share Progress**

   - Weekly/monthly updates
   - Feature highlights
   - Performance improvements

4. **Cross-Promote**
   - Link from other projects
   - Mention in talks/presentations
   - Include in your portfolio

## 📋 Promotion Checklist

- [ ] Add repository description
- [ ] Add relevant topics/tags
- [ ] Create first release
- [ ] Add badges to README
- [ ] Write blog post
- [ ] Post on Reddit (relevant subreddits)
- [ ] Share on Twitter/X
- [ ] Post on LinkedIn
- [ ] Submit to Hacker News
- [ ] Enable GitHub Discussions
- [ ] Add CONTRIBUTING.md
- [ ] Create demo video
- [ ] Engage with similar projects
- [ ] Set up website/GitHub Pages
- [ ] Create starter issues for contributors

## 💡 Pro Tips

1. **Be Patient**: Building an audience takes time
2. **Focus on Value**: Show how your project solves real problems
3. **Be Authentic**: Share your journey, not just features
4. **Engage Genuinely**: Don't just promote, participate in communities
5. **Iterate Based on Feedback**: Listen to users and improve
6. **Document Well**: Good documentation is a key differentiator
7. **Show, Don't Just Tell**: Demos and examples are powerful

## 🎓 Learn from Successful Projects

Study similar open source projects:

- How they present themselves
- Their README structure
- Their promotion strategies
- Their community engagement

---

**Remember**: The best promotion is building something valuable that solves real problems. Focus on quality, and the users will come!

Good luck with your project! 🚀

---

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

---

# Testing Framework for Media Deduplication Server

This directory contains a comprehensive testing framework for the Media Deduplication Server, including unit tests, integration tests, and performance tests.

## 🏗️ **Test Structure**

```
tests/
├── CMakeLists.txt              # Test build configuration
├── test_utils.hpp              # Test utilities header
├── test_utils.cpp              # Test utilities implementation
├── README.md                   # This file
├── scripts/                    # Test scripts directory
│   ├── README.md              # Scripts documentation
│   ├── run_tests.sh           # Test runner script
│   ├── demo_web_server.sh     # Web server demo script
│   ├── test_web_server.sh     # Web server test script
│   └── test_webserver_restart.sh # Web server restart test
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

# Or from tests/scripts directory
cd tests/scripts
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

---

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

---

# File Validation Implementation

**Date**: October 14, 2025
**Purpose**: Pre-validate RAW and TIFF files before processing to catch corrupted/unsupported files early

---

## Overview

Added validation for RAW and TIFF files using industry-standard external libraries (LibRaw, libtiff) to detect corrupt or unsupported files **before** expensive processing operations.

### Benefits

1. **Early failure detection** - Catches bad files before ImageMagick/OpenCV/ONNX processing
2. **Better error reporting** - Specific error codes (-4, -5) for validation failures
3. **Performance improvement** - Avoids wasting CPU on files that will fail anyway
4. **Reduced error logs** - Less noise from library failures on corrupt files

---

## Implementation Details

### 1. RAW File Validation (LibRaw)

**Library**: LibRaw 0.21.4  
**File**: `src/media_processors/image/backends/raw_validator.cpp`

**Validation checks**:

- File exists and is readable
- Minimum file size check (>100KB - corrupted RAW files are often truncated)
- LibRaw can open the file (`open_file()`)
- File has valid dimensions (width/height > 0)
- Camera make/model can be extracted

**Integration point**: `TranscodingPipeline::TranscodeToMemory()` - validates before ImageMagick transcoding

**Error code**: `-4` (RAW Validation Error)

**Error sources**:

- `-3`: Unsupported RAW format
- `-4`: Corrupted/truncated RAW file
- `-4`: I/O error reading file

### 2. TIFF File Validation (libtiff)

**Library**: libtiff 4.7.1  
**File**: `src/media_processors/image/backends/tiff_validator.cpp`

**Validation checks**:

- File exists and is readable
- TIFF magic bytes present (II for little-endian, MM for big-endian)
- libtiff can open the file (`TIFFOpen()`)
- File has required TIFF tags (IMAGEWIDTH, IMAGELENGTH)
- Dimensions are non-zero

**Integration points**:

- `BalancedPipeline::Run()` - validates before OpenCV ORB feature extraction
- `QualityPipeline::Run()` - validates before ONNX embedding computation

**Error code**: `-5` (TIFF Validation Error)

**Error sources**:

- `-2`: Not a TIFF file (by extension)
- `-3`: Invalid magic bytes, corrupted header, invalid IFD, missing required tags

---

## Files Created

### Headers

- `include/media_processors/image/backends/raw_validator.hpp` - RAW validation interface
- `include/media_processors/image/backends/tiff_validator.hpp` - TIFF validation interface

### Implementation

- `src/media_processors/image/backends/raw_validator.cpp` - LibRaw validation logic
- `src/media_processors/image/backends/tiff_validator.cpp` - libtiff validation logic

---

## Files Modified

### Integration

- `src/media_processors/image/backends/transcoding_pipeline.cpp` - Added RAW validation
- `src/media_processors/image/pipelines/balanced_pipeline.cpp` - Added TIFF validation
- `src/media_processors/image/pipelines/quality_pipeline.cpp` - Added TIFF validation

### Build System

- `CMakeLists.txt` - Added LibRaw and libtiff dependencies, sources, and link directories
- `tests/CMakeLists.txt` - Added validator sources to test executables and linked libraries
- `examples/CMakeLists.txt` - Added validator sources to quality_smoke example

### Documentation

- `docs/processing_status_codes.md` - Updated with new error codes (-4, -5)

---

## Dependencies

### LibRaw

- **Version**: 0.21.4
- **Location**: `/opt/homebrew/Cellar/libraw/0.21.4`
- **Library**: `libraw.dylib`
- **Purpose**: RAW format detection and validation

### libtiff

- **Version**: 4.7.1
- **Location**: `/opt/homebrew/lib`
- **Library**: `libtiff.dylib`
- **Purpose**: TIFF format validation

Both libraries were already installed via Homebrew.

---

## Error Logging

Validation failures are logged to the `processing_errors` table with:

- **Error code**: `-4` (RAW) or `-5` (TIFF)
- **Error message**: Specific validation failure reason
- **Error source**: `RawValidator` or `TiffValidator`

Example error messages:

- "RAW validation failed: Unsupported RAW format: Canon CR2 v1.0"
- "RAW validation failed: File too small to be a valid RAW file (likely truncated/corrupted)"
- "TIFF validation failed: Invalid TIFF magic bytes (corrupted or not a TIFF file)"
- "TIFF validation failed: libtiff failed to open file (corrupted TIFF structure)"

---

## Expected Impact

Based on the comprehensive report:

### RAW (CR2) Files

- **Current error rate**: 38% (6,234-6,296 failures)
- **Expected improvement**: 10-15% of failures will be caught by validation
- **Benefit**: Faster failure (~100ms validation vs 3-5s ImageMagick attempt)

### TIFF Files

- **Current error rate**: 75% (224 of 297 failures)
- **Expected improvement**: 50-70% of failures will be caught by validation
- **Benefit**: Faster failure (~10ms validation vs 500ms OpenCV attempt)

### Overall

- **Faster error detection**: Validation takes milliseconds vs seconds for full processing
- **Clearer error messages**: "TIFF validation failed: corrupted header" vs "OpenCV assertion failed"
- **Reduced processing load**: Validation avoids expensive processing operations

---

## Future Enhancements (Not Implemented)

### Configuration Toggles

Could add config properties to enable/disable validation:

- `media.validation.raw.enabled: true`
- `media.validation.tiff.enabled: true`

### Unit Tests

Could add comprehensive validator tests:

- `tests/unit/test_raw_validator.cpp` - Test various RAW formats and corruption scenarios
- `tests/unit/test_tiff_validator.cpp` - Test various TIFF formats and corruption scenarios

### Extended Validation

Could add more validation checks:

- RAW: Check for specific camera make/model compatibility
- TIFF: Validate compression schemes, color spaces
- Add validation for other formats (PNG, JPEG headers)

---

## Build Status

✅ **Build successful**

- Main server: `media_dedup_server` ✅
- All unit tests: `all_unit_tests` ✅
- Test executables: `test_media_processor`, `test_files_manager_scheduler` ✅
- Example: `quality_smoke` ✅

**Build command**: `./build.sh`  
**Executable**: `build/bin/media_dedup_server`

---

## Testing

To test the validators manually:

```bash
# Start the server
cd /Users/vinaysharma/developer/dedup_server
./build/bin/media_dedup_server

# Watch for validation errors in logs
tail -f logs/*.log | grep -i "validation"

# Check processing_errors table for validation failures
sqlite3 data/dedup_server.db "SELECT * FROM processing_errors WHERE error_source IN ('RawValidator', 'TiffValidator') ORDER BY timestamp DESC LIMIT 20;"
```

Expected outcomes:

- CR2 files with corrupted headers → `-4` error with "RAW validation failed"
- TIFF files from `/errorset/TIFF/` → `-5` error with "TIFF validation failed"
- Valid files → Pass validation, proceed to normal processing

---

_Implementation completed successfully - ready for production deployment_

---

# Duplicate Detection Tuning Guide

## Overview

This guide helps you tune the duplicate detection system for optimal accuracy based on your specific image dataset and requirements. The system uses three different modes (FAST, BALANCED, QUALITY), each with configurable parameters to balance precision (avoiding false positives) and recall (catching all true duplicates).

## Quick Start - Recommended Settings

For most use cases, use these tested configurations:

```yaml
# FAST Mode - Perceptual Hash (best for exact/near-exact duplicates)
duplicates.fast.threshold: 0.92 # 5 bits can differ out of 64
duplicates.fast.minHashSize: 64 # Standard pHash size

# BALANCED Mode - Feature Matching (best for edited/cropped versions)
duplicates.balanced.threshold: 0.35 # 35% of features must match
duplicates.balanced.minGoodMatches: 15 # At least 15 good feature matches required
duplicates.balanced.ratioTestThreshold: 0.75 # Lowe's ratio test (stricter = higher)
media.image.balanced.maxKeypoints: 1500 # Detect up to 1500 features per image

# QUALITY Mode - Deep Learning Embeddings (best for semantic similarity)
duplicates.quality.threshold: 0.96 # 96% cosine similarity required
duplicates.quality.minConfidence: 0.90 # Minimum confidence threshold

# Metadata Pre-Filtering (NEW - reduces false positives significantly!)
duplicates.metadata.filtering.enabled: true # Enable metadata filtering
duplicates.metadata.aspectRatioTolerance: 0.10 # 10% aspect ratio difference allowed
duplicates.metadata.dimensionTolerance: 0.20 # 20% dimension difference allowed
duplicates.metadata.fileSizeTolerance: 0.50 # 50% file size difference allowed
duplicates.metadata.requireSameFormat: false # Require same file extension
```

## Understanding Each Mode

### FAST Mode - Perceptual Hash (pHash)

**How it works:**

- Converts image to grayscale, resizes to 32×32
- Applies DCT (Discrete Cosine Transform)
- Generates 64-bit hash from frequency components
- Compares hashes using Hamming distance

**What it catches:**
✅ Exact duplicates
✅ Resized versions
✅ Minor compression changes
✅ Slight color adjustments

**What it misses:**
❌ Cropped images
❌ Rotated images (>15°)
❌ Heavily edited versions
❌ Images with different aspect ratios

**Threshold Guide:**

| Threshold | Bits Different | Use Case                                          |
| --------- | -------------- | ------------------------------------------------- |
| 0.95      | 3 bits         | Very strict - only near-identical                 |
| 0.92      | 5 bits         | **Recommended** - catches compression/minor edits |
| 0.90      | 6 bits         | Looser - more false positives possible            |
| 0.85      | 10 bits        | Too loose - many false positives                  |

**Example:**

- Original: IMG_001.jpg (2MB, full quality)
- Compressed: IMG_001_compressed.jpg (200KB, 80% quality)
- Similarity: ~0.94 ✅ (Caught with threshold 0.92)

---

### BALANCED Mode - ORB Feature Matching

**How it works:**

- Detects up to 1500 keypoints (corners, edges)
- Generates 256-bit ORB descriptors for each keypoint
- Matches descriptors using Hamming distance
- Applies Lowe's ratio test to filter bad matches
- Similarity = good_matches / max(features1, features2)

**What it catches:**
✅ Cropped images
✅ Rotated images
✅ Scaled images
✅ Partially occluded images
✅ Images with different borders/frames

**What it misses:**
❌ Uniform/texture-less images (not enough features)
❌ Heavily filtered images
❌ Completely different perspectives
❌ Images with < 15 features

**Parameters:**

1. **threshold** (0.35 recommended):

   - Percentage of features that must match
   - 0.30: Looser, catches more variations
   - 0.35: Balanced, good precision/recall
   - 0.40: Stricter, fewer false positives

2. **minGoodMatches** (15 recommended):

   - Absolute minimum number of good feature matches
   - Prevents false positives with feature-poor images
   - 10: Minimum for basic confirmation
   - 15: Recommended for good confidence
   - 20: Stricter, may miss some true duplicates

3. **ratioTestThreshold** (0.75 recommended):

   - Lowe's ratio test: best_dist / second_best_dist
   - 0.60: Very strict - only unambiguous matches
   - 0.75: **Recommended** - good balance
   - 0.85: Looser - more matches but less reliable

4. **maxKeypoints** (1500 recommended):
   - Maximum features to detect per image
   - 500: Fast but may miss details
   - 1000: Default balance
   - 1500: **Recommended** - better coverage
   - 2000: Slower but maximum coverage

**Example:**

- Original: DSC_0001.jpg (3000×2000)
- Cropped: DSC_0001_cropped.jpg (1500×1000, center crop)
- Features matched: 180 out of 450 = 0.40 ✅
- Good matches: 25 ✅ (exceeds minGoodMatches=15)
- Result: Caught as duplicate

---

### QUALITY Mode - CLIP Embeddings

**How it works:**

- Passes image through CLIP Vision Transformer
- Generates 512-dimensional semantic embedding
- Compares embeddings using cosine similarity
- Understands image content, not just pixels

**What it catches:**
✅ Semantically similar images
✅ Same scene, different angles
✅ Same subject, different crops
✅ Images edited heavily but same content
✅ RAW vs JPEG of same photo

**What it misses:**
❌ Nothing (most comprehensive)
❌ May group semantically similar but visually different
(e.g., different photos of same person)

**Parameters:**

1. **threshold.min** and **threshold.max** (0.94-0.98 recommended):

   - QUALITY mode uses range-based thresholds
   - **threshold.min**: Minimum similarity required (loosest match)
   - **threshold.max**: Maximum similarity for reference (future tiered matching)
   - Files with similarity ≥ threshold.min are considered duplicates

   Range examples:

   - **0.98 min, 0.98 max**: Very strict - only near-identical images
   - **0.96 min, 0.98 max**: Balanced - catches most true duplicates
   - **0.94 min, 0.98 max**: **Recommended** - comprehensive duplicate detection
   - **0.92 min, 0.98 max**: Loose - may include semantically similar images

   **Auto-reprocessing**: When threshold.min decreases (range expands), all QUALITY
   duplicate groups are automatically deleted and files are reprocessed with the new
   threshold. This allows catching more duplicates without manual intervention.

2. **minConfidence** (0.90 recommended):
   - Secondary confidence threshold
   - Reserved for future metadata filtering
   - Currently not enforced

**Example:**

- Original: IMG_5432.CR2 (RAW, 25MB, 6000×4000)
- JPEG Export: IMG_5432.jpg (JPEG, 3MB, 4000×2667, cropped + edited)
- Embedding similarity: 0.97 ✅
- Result: Caught as duplicate despite different format/size/crop

---

## Tuning Methodology

### Step 1: Start with Recommended Defaults

Use the recommended settings above and process your dataset.

### Step 2: Evaluate Results

Query your duplicate groups:

```bash
# Count groups by size
sqlite3 data/dedup_server.db "
SELECT member_count, COUNT(*) as num_groups
FROM duplicate_groups
WHERE mode='FAST'
GROUP BY member_count
ORDER BY member_count;
"

# Check largest groups (potential false positives)
sqlite3 data/dedup_server.db "
SELECT id, mode, member_count, representative_file_path
FROM duplicate_groups
ORDER BY member_count DESC
LIMIT 10;
"
```

### Step 3: Identify Issues

**Too many false positives (unrelated images grouped):**

- FAST: Increase threshold (0.92 → 0.93 → 0.94)
- BALANCED: Increase threshold (0.35 → 0.38 → 0.40)
  OR increase minGoodMatches (15 → 20 → 25)
- QUALITY: Increase threshold (0.96 → 0.97 → 0.98)

**Too many false negatives (true duplicates missed):**

- FAST: Decrease threshold (0.92 → 0.90 → 0.88)
- BALANCED: Decrease threshold (0.35 → 0.32 → 0.30)
  OR decrease minGoodMatches (15 → 12 → 10)
- QUALITY: Decrease threshold (0.96 → 0.95 → 0.94)

### Step 4: Iterative Refinement

1. Adjust one parameter at a time
2. Reset database: `./scripts/reset_duplicates.sh`
3. Restart server and wait for processing
4. Evaluate results
5. Repeat until satisfied

## Common Scenarios & Solutions

### Scenario 1: Photography Workflow

**Use case:** Photographer with RAW + JPEG exports

**Recommended settings:**

```yaml
server.mode: quality # Use QUALITY mode
duplicates.quality.threshold: 0.96 # Catches RAW/JPEG pairs
```

**Why:** CLIP embeddings understand that RAW and its JPEG export are the same image, even with different formats, sizes, and edits.

---

### Scenario 2: Downloaded Images

**Use case:** Images downloaded from internet, many duplicates with different compression

**Recommended settings:**

```yaml
server.mode: fast # Use FAST mode
duplicates.fast.threshold: 0.90 # Catches compression variations
```

**Why:** pHash is fast and catches different compression levels efficiently.

---

### Scenario 3: Edited Photos

**Use case:** Photos with crops, filters, adjustments

**Recommended settings:**

```yaml
server.mode: balanced # Use BALANCED mode
duplicates.balanced.threshold: 0.30 # Looser for crops
duplicates.balanced.minGoodMatches: 20 # Ensure good match quality
media.image.balanced.maxKeypoints: 1500 # More features for better matching
```

**Why:** Feature matching is robust to crops and minor edits.

---

### Scenario 4: Mixed Dataset

**Use case:** Mix of exact duplicates, compressed versions, and edited photos

**Recommended approach:** Run all three modes

1. Run FAST mode first (catches exact/compressed duplicates)
2. Run BALANCED mode (catches crops/edits not found by FAST)
3. Run QUALITY mode (catches semantic duplicates missed by both)

**Note:** Each mode maintains separate groups. You can query by mode.

---

## Advanced Tuning

### Precision vs Recall Trade-off

**High Precision (few false positives):**

```yaml
# Strict settings
duplicates.fast.threshold: 0.94
duplicates.balanced.threshold: 0.40
duplicates.balanced.minGoodMatches: 25
duplicates.quality.threshold: 0.98
```

**High Recall (catch all duplicates):**

```yaml
# Loose settings
duplicates.fast.threshold: 0.88
duplicates.balanced.threshold: 0.28
duplicates.balanced.minGoodMatches: 10
duplicates.quality.threshold: 0.94
```

### Dataset-Specific Tuning

**Large homogeneous dataset (similar types of images):**

- Use stricter thresholds to avoid false positives
- Increase minGoodMatches
- Use FAST mode for speed

**Small heterogeneous dataset (varied image types):**

- Use looser thresholds to catch edge cases
- Decrease minGoodMatches
- Use QUALITY mode for accuracy

**Images with text/logos:**

- Prefer BALANCED mode (features on text/logos)
- Increase maxKeypoints to 2000
- Threshold: 0.40 (stricter)

**Nature/landscape photos:**

- Prefer QUALITY mode (semantic understanding)
- Threshold: 0.95 (can be looser - scenery is distinct)

## Monitoring & Debugging

### Enable Debug Logging

```yaml
logging.level: .debug # See similarity scores
```

Look for log messages like:

- "pHash similarity: 0.9234 (hamming distance=5 bits out of 64)"
- "Feature similarity (ORB): 0.372 (25 good matches out of 450/380 features)"
- "Embedding similarity: 0.9634 (cosine similarity, dim=512)"

### Analyze Similarity Distribution

Run after processing to see similarity score distribution:

```bash
# Get similarity scores from duplicate_members table
sqlite3 data/dedup_server.db "
SELECT
    ROUND(similarity_score, 2) as score,
    COUNT(*) as count
FROM duplicate_members
WHERE is_representative = 0
GROUP BY ROUND(similarity_score, 2)
ORDER BY score DESC;
"
```

This shows how many files were matched at each similarity level.

### Identify Edge Cases

```bash
# Find groups with unusually high member counts (potential false positives)
sqlite3 data/dedup_server.db "
SELECT id, mode, member_count, representative_file_path
FROM duplicate_groups
WHERE member_count > 10
ORDER BY member_count DESC;
"
```

Manually review these large groups to check for false positives.

## Threshold Calculation Formulas

### FAST Mode - pHash

```
Hamming Distance = number of differing bits
Similarity = 1.0 - (hamming_distance / 64)

Threshold 0.92 means: 64 × (1 - 0.92) = 5.12 bits can differ
```

**Recommended range:** 4-6 bits difference (thresholds 0.90-0.94)

### BALANCED Mode - Features

```
Good Match Criteria:
  best_distance / second_best_distance < ratioTestThreshold

Similarity = good_matches / max(features1_count, features2_count)
```

**Recommended:**

- threshold: 0.30-0.40 (30-40% features match)
- minGoodMatches: 15-25 (absolute minimum)
- ratioTestThreshold: 0.70-0.80 (Lowe's ratio)

### QUALITY Mode - Embeddings

```
Cosine Similarity = (A · B) / (||A|| × ||B||)

Where A and B are 512-dimensional CLIP embeddings
```

**Recommended range:** 0.94-0.98

- < 0.94: Too loose (groups semantically similar images)
- > 0.98: Too strict (misses some true duplicates)

## Performance vs Accuracy

### Execution Speed vs Quality

| Mode         | Speed     | Accuracy | Memory | Use When                      |
| ------------ | --------- | -------- | ------ | ----------------------------- |
| **FAST**     | Very Fast | Good     | Low    | Exact/compressed duplicates   |
| **BALANCED** | Medium    | Better   | Medium | Crops/edits/transformations   |
| **QUALITY**  | Slower    | Best     | High   | RAW/JPEG, semantic duplicates |

### Improving Detection Quality

**Without sacrificing speed:**

1. Tune thresholds carefully (test on sample dataset)
2. Increase maxKeypoints for BALANCED (1500-2000)
3. Enable minGoodMatches filtering
4. Use appropriate mode for your image types

**If speed is not a concern:**

1. Use QUALITY mode for everything
2. Lower thresholds slightly
3. Process in multiple passes (FAST → BALANCED → QUALITY)

## Testing & Validation

### Create a Test Set

1. Select 10-20 known duplicate pairs from your dataset
2. Note their file paths
3. Run duplicate detection
4. Verify they were grouped correctly

### Measure Precision & Recall

**Precision** = true_positives / (true_positives + false_positives)

- How many detected duplicates are actually duplicates?
- High precision = few false positives

**Recall** = true_positives / (true_positives + false_negatives)

- How many actual duplicates were detected?
- High recall = caught most duplicates

**F1 Score** = 2 × (precision × recall) / (precision + recall)

- Balanced metric combining both

### Example Test Script

```bash
#!/bin/bash
# Test duplicate detection accuracy

# Known duplicate pairs (adjust paths to your dataset)
declare -a TEST_PAIRS=(
    "/path/to/img1.jpg:/path/to/img1_copy.jpg"
    "/path/to/img2.jpg:/path/to/img2_edited.jpg"
    "/path/to/img3.cr2:/path/to/img3.jpg"
)

# Query database for each pair
for pair in "${TEST_PAIRS[@]}"; do
    IFS=':' read -ra PATHS <<< "$pair"
    file1="${PATHS[0]}"
    file2="${PATHS[1]}"

    # Check if they're in the same group
    group1=$(sqlite3 data/dedup_server.db "
        SELECT dm.group_id FROM duplicate_members dm
        JOIN scanned_files sf ON dm.file_id = sf.id
        WHERE sf.file_path = '$file1';
    ")

    group2=$(sqlite3 data/dedup_server.db "
        SELECT dm.group_id FROM duplicate_members dm
        JOIN scanned_files sf ON dm.file_id = sf.id
        WHERE sf.file_path = '$file2';
    ")

    if [ "$group1" = "$group2" ] && [ -n "$group1" ]; then
        echo "✓ MATCH: $file1 and $file2 in group $group1"
    else
        echo "✗ MISS: $file1 (group $group1) and $file2 (group $group2)"
    fi
done
```

## Troubleshooting

### Issue: No duplicates found

**Possible causes:**

1. Thresholds too strict
2. Files not processed yet (check processing_status)
3. Artifacts missing (check image_artifacts table)

**Solutions:**

1. Lower thresholds slightly
2. Wait for processing to complete
3. Check logs for processing errors

### Issue: Too many false positives

**Possible causes:**

1. Thresholds too loose
2. Similar-looking but different images
3. Insufficient feature discriminatio

**Solutions:**

1. Increase thresholds
2. Increase minGoodMatches (BALANCED mode)
3. Switch to stricter mode (FAST → BALANCED → QUALITY)

### Issue: Large groups with unrelated images

**Possible causes:**

1. Threshold = 0 (everything matches everything!)
2. Transitive grouping bug (fixed in v3)
3. Insufficient variation in dataset

**Solutions:**

1. Check config: threshold should be 0.90-0.98, NOT 0
2. Ensure using v3 algorithm (representative-based)
3. Increase threshold to be more selective

### Issue: True duplicates not grouped

**Possible causes:**

1. Different file formats (RAW vs JPEG) with FAST mode
2. Heavy edits with FAST mode
3. Threshold too strict

**Solutions:**

1. Use QUALITY mode for cross-format duplicates
2. Use BALANCED mode for edited images
3. Lower threshold slightly

## Mode Selection Guide

### Decision Tree

```
Are images exact/near-exact copies?
├─ YES → Use FAST mode (threshold: 0.92)
└─ NO ↓

Are images cropped/rotated versions?
├─ YES → Use BALANCED mode (threshold: 0.35)
└─ NO ↓

Are images different formats/edits of same photo?
├─ YES → Use QUALITY mode (threshold: 0.96)
└─ NO → Images are probably NOT duplicates
```

### Hybrid Approach

For best results, use multiple modes in sequence:

**Pass 1: FAST mode**

- Catches exact/compressed duplicates quickly
- Threshold: 0.92

**Pass 2: BALANCED mode**

- Catches crops/edits missed by FAST
- Threshold: 0.35

**Pass 3: QUALITY mode**

- Catches semantic duplicates missed by both
- Threshold: 0.96

**Result:** Maximum coverage with good precision

## Configuration Reference

### Full Example Configuration

```yaml
# === Duplicate Detection Settings ===

# Core settings
duplicates.finder.enabled: true
duplicates.finder.batchSize: 1000
duplicates.finder.intervalMs: 3600000 # Run hourly
duplicates.finder.maxGroupSize: 100

# Representative selection
duplicates.representative.strategy: size_then_age # Larger files preferred, then older

# FAST mode - pHash
duplicates.fast.threshold: 0.92
duplicates.fast.minHashSize: 64

# BALANCED mode - ORB features
duplicates.balanced.threshold: 0.35
duplicates.balanced.minGoodMatches: 15
duplicates.balanced.ratioTestThreshold: 0.75
media.image.balanced.maxKeypoints: 1500
media.image.balanced.resizeLongEdge: 1024

# QUALITY mode - CLIP embeddings
duplicates.quality.threshold: 0.96
duplicates.quality.minConfidence: 0.90
media.image.quality.embeddingDim: 512
media.image.quality.onnx.modelPath: models/clip-image-vitb32.onnx
```

## Real-World Examples

### Example Dataset Results

**Test dataset:** 21,089 processed images

**With threshold = 0 (broken):**

- FAST: 147 groups, avg 142 members, max 764 members ❌
- Result: One massive group with unrelated images

**With threshold = 0.92 (fixed):**

- FAST: ~1,500 groups, avg 2-3 members, max 15 members ✅
- Result: Proper grouping of duplicates only

### Validation Examples

Based on your provided duplicate image paths:

**Set 1: JPG duplicates**

```
/Users/vinaysharma/Pictures/Images/JPG/Large/f12933976.jpg
/Users/vinaysharma/Pictures/Images/JPG/Large/f55318080.jpg
/Users/vinaysharma/Pictures/Images/JPG/Large/f183439128.jpg
/Users/vinaysharma/Pictures/Images/JPG/Large/f412507032.jpg
```

Expected: Should be grouped together with FAST mode (threshold 0.92)

**Set 2: CR2/RAW duplicates**

```
/Users/vinaysharma/Pictures/Images/CR2/i31159.tif.cr2
/Users/vinaysharma/Pictures/Images/Canon RAW/Valid/f387703040.cr2
/Users/vinaysharma/Pictures/Images/CR2/i28200.tif.cr2
/Users/vinaysharma/Pictures/Images/CR2/i36818.tif.cr2
```

Expected: Should be grouped with QUALITY mode (threshold 0.96)

## References

- Perceptual Hashing: [pHash Algorithm](http://www.phash.org/)
- Feature Matching: [Lowe's Ratio Test](https://www.cs.ubc.ca/~lowe/papers/ijcv04.pdf)
- CLIP Embeddings: [OpenAI CLIP](https://github.com/openai/CLIP)
- ORB Features: [OpenCV ORB Documentation](https://docs.opencv.org/4.x/d1/d89/tutorial_py_orb.html)

## Support

For issues or questions:

1. Check logs in server output
2. Query duplicate_groups table for current state
3. Review similarity scores in duplicate_members table
4. Consult main architecture doc: `docs/duplicate_detection_architecture.md`

---

**Last Updated:** October 2025  
**Version:** v3 (Representative-Based Algorithm)

---

# Media Processing Status Codes

This document defines the status codes used in the `scanned_files` table to track file processing states.

## Status Code Reference

| Code  | Description                     | Usage                                                                                                       |
| ----- | ------------------------------- | ----------------------------------------------------------------------------------------------------------- |
| `0`   | **Unprocessed**                 | File has been scanned but not yet processed                                                                 |
| `1`   | **Currently Processing**        | File is currently being processed by a worker thread                                                        |
| `2`   | **Processing Complete**         | File has been successfully processed                                                                        |
| `>2`  | **Processing Complete**         | File has been successfully processed (value indicates processing timestamp or version)                      |
| `-99` | **Queued for Processing**       | File has been submitted to the processing queue but not yet picked up by a worker thread                    |
| `-2`  | **Skipped Due to Backpressure** | File was skipped because processing queue was at capacity (retryable)                                       |
| `-1`  | **General Error**               | File processing failed due to corruption, format issues, or other general errors                            |
| `-3`  | **File Access Error**           | File processing failed due to file access issues (permission denied, file not found, file locked)           |
| `-4`  | **RAW Validation Error**        | RAW file failed LibRaw validation (unsupported format, corrupted header, truncated file)                    |
| `-5`  | **TIFF Validation Error**       | TIFF file failed libtiff validation (corrupted header, invalid IFD, missing required tags)                  |
| `-6`  | **Cache Operation Error**       | File processing failed due to disk cache issues (cache copy failed, cache save failed, cache delete failed) |

## Implementation Notes

- **Positive values** (>0): Indicate successful processing completion
- **Zero** (0): Indicates unprocessed state
- **-99**: Indicates queued for processing state
- **-98 to -1**: Indicate various failure conditions

## Error Code Rationale

The negative error codes support retry logic with escalation:

- **-99**: Queued for processing - retryable (will be picked up by worker thread)
- **-2**: Backpressure errors (queue at capacity) - retryable immediately, not escalated
- **-1**: General errors (corruption, format issues) - retryable once
- **-3**: File access errors may be temporary (file locked by another process) - retryable once
- **-4**: Memory errors may be temporary (system under load) - retryable once
- **-5**: Network errors are often temporary (network connectivity issues) - retryable once
- **-6**: Cache errors may be temporary (disk space issues, file system errors) - retryable once

## Retry Logic and Error Escalation

The system implements a **single retry with escalation** policy:

1. **Initial Error**: Files that fail processing are marked with standard error codes (-99, -2, -1, -3, -4, -5, -6)
2. **Retry Attempt**: Files with error codes >= -100 are eligible for retry
3. **Escalation**: If a file fails again after retry, the error code is escalated by subtracting 100:
   - -1 (General Error) → -101 (Escalated General Error)
   - -3 (File Access Error) → -103 (Escalated File Access Error)
   - -4 (Memory Error) → -104 (Escalated Memory Error)
   - -5 (Network Error) → -105 (Escalated Network Error)
   - -6 (Cache Error) → -106 (Escalated Cache Error)
   - -99 (Queued) → -199 (Escalated Queued - should not occur)
4. **No Further Retries**: Files with error codes < -100 are never retried again
5. **Special Case -2**: Backpressure files (-2) are retried immediately when queue capacity becomes available and are never escalated

### Escalated Error Codes

| Code   | Description                      | Retry Status                          |
| ------ | -------------------------------- | ------------------------------------- |
| `-101` | **Escalated General Error**      | No further retries                    |
| `-102` | **Escalated Backpressure Error** | No further retries (should not occur) |
| `-103` | **Escalated File Access Error**  | No further retries                    |
| `-104` | **Escalated Memory Error**       | No further retries                    |
| `-105` | **Escalated Network Error**      | No further retries                    |
| `-106` | **Escalated Cache Error**        | No further retries                    |
| `-199` | **Escalated Queued Error**       | No further retries (should not occur) |

## Error Count Reporting

The server status endpoint (`/api/v1/server/status`) reports `error_files_count` which reflects **true processing errors only**:

**Counted as Errors:**

- ✅ `-1` (General Error)
- ✅ `-3` (File Access Error)
- ✅ `-4` (Memory Error)
- ✅ `-5` (Network Error)
- ✅ `-6` (Cache Error)
- ✅ `-101` to `-106` (Escalated Errors)

**NOT Counted as Errors:**

- ❌ `-2` (Backpressure) - Transient queue capacity issue, automatically retried
- ❌ `-99` (Queued) - Files waiting to be processed, reported separately as `queued_files_count`

This distinction ensures that the error count reflects actual processing failures rather than temporary operational states. Backpressure files are automatically retried when queue capacity becomes available, and queued files are actively waiting to be processed.

## Reset Errors Operation

The "Reset All Errors" operation (`POST /api/v1/files/reset-errors`) resets error files to unprocessed status, allowing them to be retried. The operation uses the same filtering logic as error count reporting.

**Files that ARE reset (actual errors):**

- ✅ `-1` (General Error) → `0` (Unprocessed)
- ✅ `-3` (File Access Error) → `0` (Unprocessed)
- ✅ `-4` (Memory Error) → `0` (Unprocessed)
- ✅ `-5` (Network Error) → `0` (Unprocessed)
- ✅ `-6` (Cache Error) → `0` (Unprocessed)
- ✅ All escalated errors (`< -100`) → `0` (Unprocessed)

**Files that are NOT reset (temporary states):**

- ❌ `-2` (Backpressure): Temporary queue full state, will retry automatically
- ❌ `-99` (Queued): Already in processing queue
- ❌ `0` (Unprocessed): Already in correct state
- ❌ `1` (Processing): Currently being processed
- ❌ `2` (Complete): Successfully processed

**SQL Implementation:**

```sql
-- FAST mode reset
UPDATE scanned_files SET processed_fast=0
WHERE processed_fast < 0 AND processed_fast != -2 AND processed_fast != -99;

-- BALANCED mode reset
UPDATE scanned_files SET processed_balanced=0
WHERE processed_balanced < 0 AND processed_balanced != -2 AND processed_balanced != -99;

-- QUALITY mode reset
UPDATE scanned_files SET processed_quality=0
WHERE processed_quality < 0 AND processed_quality != -2 AND processed_quality != -99;
```

This ensures consistency with error count reporting and preserves operational states that are not actual errors.

## Database Schema

The `scanned_files` table uses the `status` column to store these codes:

```sql
CREATE TABLE scanned_files (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    file_path TEXT UNIQUE NOT NULL,
    status INTEGER NOT NULL DEFAULT 0,
    -- ... other columns
);
```

## Query Examples

```sql
-- List all unprocessed files (including retryable errors and backpressure)
SELECT file_path FROM scanned_files WHERE processed_fast = 0 OR processed_fast = -99 OR (processed_fast >= -100 AND processed_fast < 0);

-- List files that failed due to file access issues
SELECT file_path FROM scanned_files WHERE processed_fast = -3;

-- List files that need retry (all retryable errors >= -100, including backpressure)
SELECT file_path FROM scanned_files WHERE processed_fast = -99 OR (processed_fast >= -100 AND processed_fast < 0);

-- List files currently queued for processing
SELECT file_path FROM scanned_files WHERE processed_fast = -99;

-- List files that will never be retried (escalated errors < -100)
SELECT file_path FROM scanned_files WHERE processed_fast < -100;

-- List backpressure files (retryable immediately)
SELECT file_path FROM scanned_files WHERE processed_fast = -2;

-- List files with specific escalated errors
SELECT file_path FROM scanned_files WHERE processed_fast = -101; -- Escalated general error
SELECT file_path FROM scanned_files WHERE processed_fast = -103; -- Escalated file access error
```

---

## Error Message Persistence

### Processing Errors Table

The system maintains a separate `processing_errors` table to record detailed error information when files fail processing. This provides historical context and debugging information beyond the status codes stored in `scanned_files`.

#### Table Schema

```sql
CREATE TABLE processing_errors (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    file_path TEXT NOT NULL,
    server_mode TEXT NOT NULL,  -- 'FAST', 'BALANCED', 'QUALITY'
    error_code INTEGER NOT NULL,
    error_message TEXT NOT NULL,
    error_source TEXT,  -- 'ImageMagick', 'OpenCV', 'ONNX', 'Memory', 'FileSystem', 'Network', 'General'
    timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Indexes for efficient queries
CREATE INDEX idx_processing_errors_file_path ON processing_errors(file_path);
CREATE INDEX idx_processing_errors_timestamp ON processing_errors(timestamp DESC);
```

#### Error Sources

| Source        | Description                     |
| ------------- | ------------------------------- |
| `ImageMagick` | RAW file transcoding errors     |
| `OpenCV`      | Image reading/processing errors |
| `ONNX`        | Neural network inference errors |
| `Memory`      | Memory allocation failures      |
| `FileSystem`  | File access/permission errors   |
| `Network`     | Network-related errors          |
| `General`     | Other unclassified errors       |

#### Usage

Errors are automatically logged when files fail processing. Multiple error records can exist for the same file across different processing attempts and modes.

#### Query Examples

```sql
-- Get all error details for a specific file
SELECT * FROM processing_errors
WHERE file_path = '/path/to/file.jpg'
ORDER BY timestamp DESC;

-- Find files with OpenCV errors in QUALITY mode
SELECT DISTINCT file_path, error_message
FROM processing_errors
WHERE error_source = 'OpenCV' AND server_mode = 'QUALITY'
ORDER BY timestamp DESC LIMIT 100;

-- Count errors by source in the last 24 hours
SELECT error_source, COUNT(*) as error_count
FROM processing_errors
WHERE timestamp > datetime('now', '-24 hours')
GROUP BY error_source
ORDER BY error_count DESC;

-- Get detailed error information for escalated errors (-101)
SELECT sf.file_path, sf.processed_quality, pe.error_message, pe.error_source, pe.timestamp
FROM scanned_files sf
JOIN processing_errors pe ON sf.file_path = pe.file_path
WHERE sf.processed_quality = -101 AND pe.server_mode = 'QUALITY'
ORDER BY pe.timestamp DESC;

-- Find most common error messages
SELECT error_message, COUNT(*) as occurrences
FROM processing_errors
WHERE timestamp > datetime('now', '-7 days')
GROUP BY error_message
ORDER BY occurrences DESC
LIMIT 20;
```

#### Design Rationale

- **Separation of Concerns**: Error details are kept separate from operational state (`scanned_files`)
- **Historical Record**: All error attempts are preserved, not just the latest
- **Write-Optimized**: Insert-only operations, no foreign key constraints for maximum performance
- **Debugging**: Detailed error messages from exceptions captured for post-event analysis
- **No Impact on Processing**: Error logging failures don't block file processing

