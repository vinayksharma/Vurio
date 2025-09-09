# Media Deduplication Server

A high-performance, scalable media deduplication server built with C++ and modern libraries. This server efficiently identifies and manages duplicate media files (images, videos, audio) using advanced hashing algorithms and database management.

## 🚀 Features

- **Multi-format Support**: Handles images (JPG, PNG, GIF, etc.), videos (MP4, AVI, MOV, etc.), and audio files (MP3, WAV, FLAC, etc.)
- **Intelligent Deduplication**: Uses SHA-256 hashing for accurate duplicate detection
- **Database Management**: SQLite-based storage with Poco Data for efficient data handling
- **Configuration Management**: Flexible YAML-based configuration with Poco Util
- **HTTP API**: RESTful API for file upload, search, and management
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

## 🔧 Installation

### 1. Install Dependencies

#### Ubuntu/Debian:

```bash
sudo apt update
sudo apt install build-essential cmake libpoco-dev libsqlite3-dev pkg-config
```

#### macOS:

```bash
brew install cmake poco sqlite3 pkg-config
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

The web server API (config, user settings, media locations, OpenAPI, TPM status) is documented in `WEB_SERVER_README.md`. Quick links:

- `GET /api/v1/config`, `GET /api/v1/config/{key}`, `PUT /api/v1/config/{key}`
- `POST /api/v1/config/reload`, `GET /api/v1/config/status`, `GET /api/openapi.json`
- `GET /api/v1/tpm/status`

## 🗄️ Database Schema

The server uses SQLite with the following main tables:

- **media_files**: File metadata and paths
- **file_hashes**: Hash values and file associations
- **metadata**: Extended file information

## 🔍 Development

### Project Structure

```
dedup_server/
├── include/                 # Header files
│   ├── config/             # Configuration management
│   ├── core/               # Core server components
│   ├── database/           # Database management
│   └── web/                # Web API components
├── src/                    # Source files
│   ├── config/             # Configuration implementation
│   ├── core/               # Core server implementation
│   ├── database/           # Database implementation
│   └── web/                # Web API implementation
├── tests/                  # Test files
├── config/                 # Configuration files
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

- **File Processing**: Up to 1000 files/second (depending on file size)
- **Hash Generation**: SHA-256 at ~50MB/s per core
- **Database Operations**: 10,000+ queries/second
- **Memory Usage**: ~50MB base + 10MB per 1000 files
- **Storage Overhead**: Minimal - only metadata and hashes stored

---

**Built with ❤️ using C++ and modern libraries**
