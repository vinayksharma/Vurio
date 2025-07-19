# Dedup Service

A secure dedup service that provides status checking through a REST API with JWT authentication.

## Features

- Service status checking
- JWT-based authentication
- RESTful API endpoints
- Unit tests
- Modern C++ (C++17)
- Multi-mode deduplication (FAST, BALANCED, QUALITY)

### Deduplication Modes

The server supports three deduplication modes optimized for different use cases:

- **FAST Mode**: OpenCV dHash + FFmpeg - Fast scanning, acceptable quality, low resource use
- **BALANCED Mode**: libvips + OpenCV pHash + FFmpeg - Good balance of speed and accuracy
- **QUALITY Mode**: ONNX Runtime + CNN embeddings + FFmpeg - Highest accuracy, computationally intensive (GPU recommended)

_Note: Deduplication functionality is currently in development and will be implemented in future versions._

## Dependencies

- CMake (3.10 or higher)
- C++17 compatible compiler
- OpenSSL
- nlohmann_json
- Google Test (automatically downloaded by CMake)
- cpp-httplib (automatically downloaded by CMake)
- jwt-cpp (automatically downloaded by CMake)

### Deduplication Libraries (Required for all modes)

- **OpenCV** - Image processing and perceptual hashing (dHash, pHash)
- **FFmpeg** - Video processing and key frame extraction
- **libvips** - High-performance image processing (BALANCED mode)
- **ONNX Runtime** - Neural network inference (QUALITY mode)

### Installation Notes

```bash
# Ubuntu/Debian
sudo apt-get install libopencv-dev libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libvips-dev

# macOS
brew install opencv ffmpeg vips onnxruntime

# CentOS/RHEL
sudo yum install opencv-devel ffmpeg-devel vips-devel
```

## Building

```bash
mkdir build
cd build
cmake ..
make
```

## Running Tests

```bash
cd build
ctest
```

## Running the Server

```bash
cd build
./dedup_server
```

The server will start on `localhost:8080`.

## API Endpoints

### Authentication

- `POST /auth/login`
  - Request body: `{"username": "string", "password": "string"}`
  - Response: `{"token": "string"}`

### Status Operations

All status endpoints require a valid JWT token in the `Authorization` header:
`Authorization: Bearer <token>`

- `GET /auth/status`
  - Response: `{"status": boolean}`

### Duplicate Finding Operations

All duplicate finding endpoints require a valid JWT token in the `Authorization` header:
`Authorization: Bearer <token>`

- `POST /duplicates/find`
  - Request body: `{"directory": "string"}`
  - Response: `{"message": "string"}`
  - Scans directory recursively and prints found files to console

### Configuration Operations

All configuration endpoints require a valid JWT token in the `Authorization` header:
`Authorization: Bearer <token>`

- `GET /config`

  - Response: Complete server configuration JSON
  - Returns current server settings including dedup mode, log level, etc.

- `PUT /config`

  - Request body: Partial or complete configuration JSON
  - Response: `{"message": "Configuration updated successfully"}`
  - Updates server configuration and triggers reactive events

- `POST /config/reload`

  - Request body: `{"file_path": "string"}`
  - Response: `{"message": "Configuration reloaded successfully"}`
  - Reloads configuration from specified file

- `POST /config/save`
  - Request body: `{"file_path": "string"}`
  - Response: `{"message": "Configuration saved successfully"}`
  - Saves current configuration to specified file

## Configuration

The server supports both file-based and hardcoded configuration. By default, the server will:

1. **Try to load `config.json`** from the current directory
2. **Create a default `config.json`** if the file doesn't exist
3. **Fall back to hardcoded defaults** if file creation fails

### Configuration File Format

The server will automatically create a `config.json` file with default values if none exists:

```json
{
  "dedup_mode": "BALANCED",
  "log_level": "INFO",
  "server_port": 8080,
  "auth_secret": "your-secret-key-here",
  "server_host": "localhost"
}
```

### Configuration Fields

- **`dedup_mode`**: Processing mode (`FAST`, `BALANCED`, `QUALITY`)
- **`log_level`**: Logging level (`TRACE`, `DEBUG`, `INFO`, `WARN`, `ERROR`)
- **`server_port`**: HTTP server port (1-65535)
- **`auth_secret`**: JWT secret key for authentication
- **`server_host`**: Server hostname

### Runtime Configuration Management

You can also manage configuration at runtime via the API:

- **Get current configuration**: `GET /api/config`
- **Update configuration**: `PUT /api/config`
- **Reload from file**: `POST /api/config/reload`
- **Save to file**: `POST /api/config/save`

## Example Usage

1. Get a token:

```bash
curl -X POST http://localhost:8080/auth/login \
  -H "Content-Type: application/json" \
  -d '{"username": "testuser", "password": "testpass"}'
```

2. Check service status:

```bash
curl -X GET http://localhost:8080/auth/status \
  -H "Authorization: Bearer <token>"
```

3. Find duplicates in a directory:

```bash
curl -X POST http://localhost:8080/duplicates/find \
  -H "Authorization: Bearer <token>" \
  -H "Content-Type: application/json" \
  -d '{"directory": "/path/to/scan"}'
```

4. Get server configuration:

```bash
curl -X GET http://localhost:8080/config \
  -H "Authorization: Bearer <token>"
```

5. Update server configuration:

```bash
curl -X PUT http://localhost:8080/config \
  -H "Authorization: Bearer <token>" \
  -H "Content-Type: application/json" \
  -d '{"dedup_mode": "FAST", "log_level": "DEBUG"}'
```

6. Save configuration to file:

```bash
curl -X POST http://localhost:8080/config/save \
  -H "Authorization: Bearer <token>" \
  -H "Content-Type: application/json" \
  -d '{"file_path": "/path/to/config.json"}'
```

## Security Notes

### Current Development State (NOT PRODUCTION READY)

- **Hardcoded credentials**: username: "testuser", password: "testpass"
- **Hardcoded JWT secret**: Tokens persist across server restarts
- **No token expiration**: JWT tokens never expire
- **No password hashing**: Passwords stored in plain text
- **No rate limiting**: No protection against brute force attacks

### Production Requirements

- Use a secure secret key for JWT signing from environment variables
- Store the secret key in environment variables
- Implement proper user authentication against a database
- Add JWT token expiration (24 hours recommended)
- Hash passwords with bcrypt/argon2
- Use HTTPS in production
- Implement rate limiting and other security measures
- Add audit logging for authentication events
- Consider token blacklisting/revocation mechanisms
