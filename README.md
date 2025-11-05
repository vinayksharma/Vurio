# Media Deduplication Server

A high-performance, scalable media deduplication server built with C++ and modern libraries. This server efficiently identifies and manages duplicate media files (images, videos, audio) using advanced hashing algorithms and database management.

## 🚀 Features

- **Multi-format Support**: Handles images (JPG, PNG, GIF, etc.), videos (MP4, AVI, MOV, etc.), and audio files (MP3, WAV, FLAC, etc.)
- **Intelligent Deduplication**: Uses SHA-256 hashing for accurate duplicate detection
- **Database Management**: SQLite-based storage with Poco Data for efficient data handling
- **Unified Configuration System**: Observable YAML-based configuration with live updates
- **RESTful HTTP API**: Static OpenAPI 3.0 specification with 13 endpoints
- **Thread Pool Management**: Configurable thread pools with per-type resource allocation
- **Scheduler Service**: Advanced job scheduling with jitter, backoff, and drift management
- **File Management**: Automated file scanning and media location monitoring
- **Instance Management**: Prevents multiple server instances from running simultaneously
- **Console Interface**: Interactive command-line interface with graceful shutdown
- **Performance Optimized**: Asynchronous processing, caching, and connection pooling
- **Extensible Architecture**: Modular design for easy feature additions

## 🏗️ Architecture

The server is built with a modular, layered architecture:

```
┌─────────────────────────────────────────────────────────────┐
│                    HTTP API Layer                           │
├─────────────────────────────────────────────────────────────┤
│                  Business Logic Layer                       │
├─────────────────────────────────────────────────────────────┤
│                Media Processing Layer                       │
├─────────────────────────────────────────────────────────────┤
│                  Database Layer                             │
├─────────────────────────────────────────────────────────────┤
│                Configuration Layer                           │
└─────────────────────────────────────────────────────────────┘
```

## 📋 Prerequisites

- **C++17** compatible compiler (GCC 7+, Clang 5+, MSVC 2017+)
- **CMake 3.16+**
- **Poco Libraries** (Foundation, Data, SQLite, Util, Net)
- **SQLite3** development libraries
- **yaml-cpp** (YAML configuration parsing)
- **ImageMagick++** (Image processing and RAW file support)
- **LibRaw** (RAW image file validation)
- **libtiff** (TIFF file validation)
- **pkg-config** (for dependency detection)
- **Python 3** (for ONNX model downloading)
- **huggingface_hub** Python package (for automatic model downloads)
- **curl or wget** (for model downloads)

## 🔧 Installation

### 1. Install Dependencies

#### macOS (Recommended)

**Prerequisites:**
- [Homebrew](https://brew.sh/) package manager
- Xcode Command Line Tools: `xcode-select --install`

**Install all dependencies:**

```bash
# Install Homebrew if not already installed
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# Install all required dependencies
brew install cmake poco sqlite3 pkg-config imagemagick libraw libtiff yaml-cpp

# Install Python dependencies for ONNX models
pip3 install huggingface_hub
```

**Note for Apple Silicon (M1/M2/M3):**
- Homebrew installs to `/opt/homebrew` by default
- The build system will automatically detect dependencies in `/opt/homebrew`
- If you have an Intel Mac, dependencies will be in `/usr/local` (also auto-detected)

#### Ubuntu/Debian:

```bash
sudo apt update
sudo apt install build-essential cmake libpoco-dev libsqlite3-dev \
    pkg-config libyaml-cpp-dev libmagick++-dev libraw-dev libtiff-dev \
    python3-pip
pip3 install huggingface_hub
```

#### CentOS/RHEL:

```bash
sudo yum install gcc-c++ cmake3 poco-devel sqlite-devel pkgconfig \
    yaml-cpp-devel ImageMagick-c++-devel libraw-devel libtiff-devel \
    python3-pip
pip3 install huggingface_hub
```

### 2. Clone and Build

```bash
# Clone the repository
git clone <repository-url>
cd dedup_server

# Make build script executable
chmod +x build.sh

# Build the project (recommended)
./build.sh

# Or build with tests
./build.sh --clean --test

# Or manual CMake build
mkdir build && cd build
cmake ..
make -j$(sysctl -n hw.ncpu)  # macOS: use all CPU cores
```

**Build Options:**
- `./build.sh` - Standard release build
- `./build.sh --clean` - Clean build (removes old artifacts)
- `./build.sh --test` - Build and run tests
- `./build.sh --clean --test` - Clean build and run all tests
- `./build.sh --debug` - Debug build with symbols
- `./build.sh --help` - Show all options

**Build Output:**
- Executable: `build/bin/media_dedup_server`
- Test executable: `build/bin/all_unit_tests`
- Configuration: `config/config.yaml`

**💡 For development with IDE:** See [Build Tasks Documentation](docs/BUILD_TASKS.md) for VS Code/Cursor build tasks and keyboard shortcuts (Cmd+Shift+B).

### 3. Install

```bash
sudo make install
```

## 🤖 ONNX Models (Optional)

The server includes advanced image processing capabilities using ONNX models for deep learning-based deduplication. These models are **automatically downloaded** during the build process if ONNX Runtime is available.

### Automatic Model Download

When you build the project, CMake will automatically:

- Download CLIP ViT-B/32 model (~350MB) for quality image processing
- Download CLIP RN50 model (~600MB) for alternative quality processing
- Place models in the `models/` directory
- Configure the server to use these models

### Manual Model Management

If you need to manage models manually:

```bash
# Download all models
make download_models

# Download specific models
make download_clip_vitb32
make download_clip_rn50

# Use custom model URLs
python3 scripts/fetch_clip_from_hub.py
./scripts/fetch_clip_rn50_onnx.sh <MODEL_URL>
```

### Model Requirements

- **CLIP ViT-B/32**: `models/clip-image-vitb32.onnx` (~350MB)
- **CLIP RN50**: `models/clip-RN50.onnx` (~600MB)

The server will work without these models, but the Quality image processing pipeline will be disabled.

## ⚙️ Configuration

See `config/CONFIGURATION_REFERENCE.md` for the canonical list of configuration keys, defaults, and live effects. A minimal sample is provided in `config/config.yaml` and includes TPM defaults (`tpm.pool.max`, `tpm.killTimeoutMs`) and examples for per-type shares (`tpm.types.<name>.share`).

## 🚀 Usage

### Running the Server

#### Quick Start (macOS)

```bash
# Start server with default configuration
./build/bin/media_dedup_server

# Or use the convenience script (builds if needed)
./start

# Build and start in one command
./start --build
```

#### Using the Start Script

The `start` script provides an easy way to run the server:

```bash
# Start with default configuration
./start

# Build first, then start
./start --build

# Start in debug mode
./start --debug

# Start on a specific port
./start --port 9090

# Start on localhost only
./start --host localhost

# Use a custom config file
./start --config /path/to/config.yaml
```

#### Direct Server Execution

```bash
# Start with default configuration
./build/bin/media_dedup_server

# Start with custom config file
./build/bin/media_dedup_server --config=config/config.yaml

# Start with custom database path
./build/bin/media_dedup_server --database=data/dedup_server.db

# Start on specific host and port
./build/bin/media_dedup_server --host=localhost --port=8080

# Show help
./build/bin/media_dedup_server --help
```

### Command Line Options

- `--config=<file>`: Specify configuration file path (default: `config/config.yaml`)
- `--database=<path>`: Specify database file path (default: `data/dedup_server.db`)
- `--host=<address>`: Server host address (default: `0.0.0.0`)
- `--port=<number>`: Server port number (default: `8080`)
- `--help`: Show help information

### Accessing the Web Interface

Once the server is running, you can access:

- **Web API**: `http://localhost:8080/api/v1/config`
- **OpenAPI Spec**: `http://localhost:8080/api/openapi.json`
- **Swagger UI**: `http://localhost:8080/` (if available)

### Stopping the Server

- Press `Ctrl+C` in the terminal
- Or type `exit` in the console interface
- Or send `SIGTERM` signal: `kill <pid>`

## 📊 Web API

The web server provides a comprehensive RESTful API with 13 endpoints for configuration management, user settings, media locations, and system monitoring. Full documentation is available in `WEB_SERVER_README.md`.

### Configuration Management

- `GET /api/v1/config` - Get all configuration properties
- `GET /api/v1/config/{key}` - Get specific configuration property
- `PUT /api/v1/config/{key}` - Update configuration property
- `POST /api/v1/config/reload` - Reload configuration from file
- `GET /api/v1/config/status` - Get system status

### User Settings & Media Locations

- `GET /api/v1/user-settings` - Get all user settings
- `GET /api/v1/user-settings/{key}` - Get specific user setting
- `PUT /api/v1/user-settings/{key}` - Create/update user setting
- `DELETE /api/v1/user-settings/{key}` - Delete user setting
- `POST /api/v1/media-locations/register` - Register media location
- `POST /api/v1/media-locations/deregister` - Deregister media location

### System Monitoring

- `GET /api/v1/tpm/status` - Get Thread Pool Manager status
- `GET /api/openapi.json` - OpenAPI 3.0 specification

## 🗄️ Database Schema

The server uses SQLite with the following main tables:

- **user_settings**: Key-value user settings (`key TEXT PRIMARY KEY, value TEXT NOT NULL`)
- **scanned_files**: File metadata and processing status with fields for different processing modes
- **Additional tables**: Created dynamically as needed for media processing

## 🔍 Development

### Project Structure (Key Paths)

```
dedup_server/
├── include/                 # Header files
│   ├── config/             # Configuration management
│   ├── core/               # Core server components
│   ├── database/           # Database management
│   ├── filesmanager/       # File scanning and management
│   └── orchestration/      # Thread pool and scheduler
├── src/                    # Source files
│   ├── config/             # Configuration implementation
│   ├── core/               # Core server implementation
│   ├── database/           # Database implementation
│   ├── filesmanager/       # File scanning implementation
│   ├── orchestration/      # Thread pool and scheduler implementation
│   └── webserver/          # Web API handlers and static files
│       └── static/         # Static HTML/CSS/JS and OpenAPI spec
├── tests/                  # Test files
├── config/                 # Configuration files
├── scripts/                # Utility scripts
├── CMakeLists.txt          # Build configuration
└── README.md               # This file
```

### Building for Development

```bash
# Debug build
mkdir build-debug && cd build-debug
cmake -DCMAKE_BUILD_TYPE=Debug ..
make -j$(sysctl -n hw.ncpu)  # macOS: use all CPU cores
# Linux: use make -j$(nproc)

# Or use the build script
./build.sh --debug
```

### Running Tests

```bash
# Run all tests (after building)
cd build
./bin/all_unit_tests

# Or use the build script with test flag
./build.sh --test

# Or clean build and test
./build.sh --clean --test
```

**Test Output:**
- Test results: `build/test_results.xml`
- Test executable: `build/bin/all_unit_tests`

## 🤝 Contributing

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/amazing-feature`)
3. Commit your changes (`git commit -m 'Add amazing feature'`)
4. Push to the branch (`git push origin feature/amazing-feature`)
5. Open a Pull Request

## 📝 License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## 🆘 Support & Troubleshooting

### Common macOS Issues

**Issue: "Poco not found" or "yaml-cpp not found"**
```bash
# Ensure Homebrew is properly installed and in PATH
eval "$(/opt/homebrew/bin/brew shellenv)"  # Apple Silicon
# or
eval "$(/usr/local/bin/brew shellenv)"     # Intel

# Verify dependencies are installed
brew list | grep -E "(poco|yaml-cpp|imagemagick|libraw|libtiff)"
```

**Issue: "ImageMagick not found"**
```bash
# Install ImageMagick via Homebrew
brew install imagemagick

# Verify installation
pkg-config --modversion Magick++
```

**Issue: Build fails with "libtiff not found"**
```bash
# Install libtiff
brew install libtiff

# Verify installation
pkg-config --modversion libtiff-4
```

**Issue: "Permission denied" when running build.sh**
```bash
# Make script executable
chmod +x build.sh
chmod +x start
```

**Issue: Python dependencies not found**
```bash
# Use pip3 explicitly
pip3 install --user huggingface_hub

# Or install globally (requires sudo)
sudo pip3 install huggingface_hub
```

### Getting Help

- **Issues**: Use [GitHub Issues](https://github.com/your-repo/issues)
- **Documentation**: 
  - `docs/CONFIGURATION_REFERENCE.md` - Complete configuration reference
  - `docs/WEB_SERVER_README.md` - Web API documentation
  - `docs/START_SCRIPT_README.md` - Start script usage
  - `docs/BUILD_TASKS.md` - IDE build configuration

## 🔮 Roadmap

- [ ] Web-based management interface
- [ ] Advanced media analysis (content-based deduplication)
- [ ] Cloud storage integration
- [ ] Machine learning-based duplicate detection
- [ ] Real-time file monitoring
- [ ] Multi-node clustering support
- [ ] Plugin system for custom processors

## 📊 Performance

- **File Processing**: Configurable scanning with scheduler-based processing
- **Thread Management**: Auto-detected or configurable thread pools with per-type resource allocation
- **Database Operations**: Connection pooling with configurable timeouts and backoff
- **Memory Usage**: Efficient memory management with configurable limits
- **Storage Overhead**: Minimal - only metadata and processing status stored
- **Configuration**: Live updates without server restart for most settings
- **Scheduling**: Advanced job scheduling with jitter, backoff, and drift management

---

**Built with ❤️ using C++ and modern libraries**
