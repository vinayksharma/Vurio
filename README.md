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
- **pkg-config** (for dependency detection)
- **Python 3** (for ONNX model downloading)
- **huggingface_hub** Python package (for automatic model downloads)
- **curl or wget** (for model downloads)

## 🔧 Installation

### 1. Install Dependencies

#### Ubuntu/Debian:

```bash
sudo apt update
sudo apt install build-essential cmake libpoco-dev libsqlite3-dev pkg-config python3-pip
pip3 install huggingface_hub
```

#### macOS:

```bash
brew install cmake poco sqlite3 pkg-config
pip3 install huggingface_hub
```

#### CentOS/RHEL:

```bash
sudo yum install gcc-c++ cmake3 poco-devel sqlite-devel pkgconfig
```

### 2. Clone and Build

```bash
git clone <repository-url>
cd dedup_server
mkdir build && cd build
cmake ..
make -j$(nproc)
```

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

### Basic Server Startup

```bash
# Start with default configuration
./media_dedup_server

# Start with custom config file
./media_dedup_server --config=/path/to/config.yaml

# Start in daemon mode
./media_dedup_server --daemon

# Show help
./media_dedup_server --help
```

### Command Line Options

- `--config=<file>`: Specify configuration file path
- `--database=<path>`: Specify database file path
- `--host=<address>`: Server host address
- `--port=<number>`: Server port number
- `--daemon`: Run in daemon mode
- `--help`: Show help information

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
mkdir build-debug && cd build-debug
cmake -DCMAKE_BUILD_TYPE=Debug ..
make -j$(nproc)
```

### Running Tests

```bash
cd build
make test
```

## 🤝 Contributing

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/amazing-feature`)
3. Commit your changes (`git commit -m 'Add amazing feature'`)
4. Push to the branch (`git push origin feature/amazing-feature`)
5. Open a Pull Request

## 📝 License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## 🆘 Support

- Issues: use GitHub Issues
- Documentation: `config/CONFIGURATION_REFERENCE.md`, `WEB_SERVER_README.md`, `START_SCRIPT_README.md`

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
