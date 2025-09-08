# Media Deduplication Server Web API

This document describes the web server component of the Media Deduplication Server, which provides a RESTful HTTP API for configuration management.

## Overview

The web server integrates with the unified observable configuration system to provide:

- **HTTP endpoints** for configuration management
- **Real-time configuration updates** via the unified observable system
- **OpenAPI specification** for API documentation
- **CORS support** for web applications
- **JSON-based communication** for easy integration

## Architecture

```
┌─────────────────┐    HTTP Requests    ┌──────────────────┐    Configuration    ┌─────────────────────┐
│   HTTP Client   │ ──────────────────► │   Web Server     │ ──────────────────► │ Unified Observable  │
│                 │                     │                  │                     │ Config Manager      │
└─────────────────┘                     └──────────────────┘                     └─────────────────────┘
                                                │
                                                ▼
                                        ┌──────────────────┐
                                        │ Configuration    │
                                        │ File (YAML)      │
                                        └──────────────────┘
```

## Features

### 🔄 **Bidirectional Configuration Updates**

- **HTTP API** → **Configuration File**: Updates via PUT requests are automatically saved
- **Configuration File** → **HTTP API**: File changes are automatically detected and loaded
- **Real-time synchronization** between API, memory, and file system

### 🌐 **RESTful API Design**

- **Standard HTTP methods** (GET, PUT, POST)
- **Consistent response format** with proper HTTP status codes
- **JSON request/response bodies** for easy integration

### 📚 **OpenAPI Documentation**

- **Self-documenting API** via `/api/openapi.json`
- **Interactive documentation** can be generated using Swagger UI
- **Complete endpoint specifications** with request/response schemas

### 🔒 **CORS Support**

- **Cross-origin requests** supported for web applications
- **Configurable CORS headers** for security
- **Preflight OPTIONS requests** handled automatically

## API Endpoints

### 1. **GET /api/v1/config**

Retrieves all configuration properties.

**Response:**

```json
{
  "server.host": "0.0.0.0",
  "server.port": 8080,
  "logging.level": "info",
  "database.path": "data/dedup_server.db"
}
```

### 2. **GET /api/v1/config/{key}**

Retrieves a specific configuration property.

**Parameters:**

- `key` (path): Configuration property key

**Response:**

```json
{
  "key": "logging.level",
  "value": "info",
  "description": "Logging level",
  "modified": false
}
```

### 3. **PUT /api/v1/config/{key}**

Updates a configuration property.

**Parameters:**

- `key` (path): Configuration property key

**Request Body:**

```json
{
  "value": "debug"
}
```

**Response:**

```json
{
  "message": "Property updated successfully",
  "key": "logging.level",
  "value": "debug"
}
```

### 4. **POST /api/v1/config/reload**

Reloads configuration from the file system.

**Response:**

```json
{
  "message": "Configuration reloaded successfully"
}
```

### 5. **GET /api/v1/config/status**

Retrieves the configuration system status.

**Response:**

```json
{
  "valid": true,
  "config_file": "config/config.yaml",
  "property_count": 8,
  "validation_errors": []
}
```

### 6. **GET /api/openapi.json**

Retrieves the OpenAPI 3.0 specification.

**Response:** Complete OpenAPI specification in JSON format.

### 7. **User Settings API**

CRUD for simple key/value user settings persisted in SQLite.

#### GET /api/v1/user-settings

Returns all settings as a flat object.

#### GET /api/v1/user-settings/{key}

Returns a single setting by key.

#### PUT /api/v1/user-settings/{key}

Create or update a setting.

Request body:

```json
{ "value": "string" }
```

#### DELETE /api/v1/user-settings/{key}

Deletes a setting by key.

### 8. **Media Locations API**

Register and deregister media library directories.

#### POST /api/v1/media-locations/register

Body:

```json
{ "directory": "/path/to/media" }
```

Response:

```json
{ "status": "ok", "directory": "/path/to/media" }
```

#### POST /api/v1/media-locations/deregister

Body:

```json
{ "directory": "/path/to/media" }
```

Response:

```json
{ "status": "ok", "directory": "/path/to/media" }
```

## Configuration Reference

For the authoritative configuration keys, defaults, and live update behavior, see `config/CONFIGURATION_REFERENCE.md`.

## Usage Examples

### **Starting the Server**

```bash
# Build the project
./build.sh

# Start the server
./build/bin/media_dedup_server
```

### **Testing the API**

```bash
# Run the full demo (starts server and tests all endpoints)
./demo_web_server.sh

# Start server only
./demo_web_server.sh --start

# Test API endpoints only (server must be running)
./demo_web_server.sh --test
```

### **Using curl**

```bash
# Get all configuration
curl http://localhost:8080/api/v1/config

# Get specific property
curl http://localhost:8080/api/v1/config/logging.level

# Update property
curl -X PUT http://localhost:8080/api/v1/config/logging.level \
  -H "Content-Type: application/json" \
  -d '{"value": "debug"}'

# Get system status
curl http://localhost:8080/api/v1/config/status

# Get OpenAPI spec
curl http://localhost:8080/api/openapi.json
```

### **Using JavaScript/Fetch**

```javascript
// Get all configuration
const response = await fetch("http://localhost:8080/api/v1/config");
const config = await response.json();

// Update property
const updateResponse = await fetch(
  "http://localhost:8080/api/v1/config/logging.level",
  {
    method: "PUT",
    headers: {
      "Content-Type": "application/json",
    },
    body: JSON.stringify({ value: "debug" }),
  }
);

// Get property details
const propertyResponse = await fetch(
  "http://localhost:8080/api/v1/config/server.port"
);
const property = await propertyResponse.json();
```

## Integration with Unified Observable System

The web server is tightly integrated with the unified observable configuration system:

### **Automatic Property Creation**

- New properties added to the configuration file are automatically discovered
- Properties can be created via API calls and immediately persisted
- All property changes trigger appropriate events and notifications

### **Real-time Updates**

- Configuration file changes are automatically detected and loaded
- API updates are immediately reflected in the file system
- Bidirectional synchronization prevents data loss

### **Event-driven Architecture**

- Configuration changes emit events for other system components
- File system changes trigger reload events
- Property updates notify all subscribers automatically

## Error Handling

The API provides comprehensive error handling:

### **HTTP Status Codes**

- `200 OK`: Request successful
- `400 Bad Request`: Invalid request (missing required fields, invalid data)
- `404 Not Found`: Configuration property not found
- `405 Method Not Allowed`: HTTP method not supported for endpoint
- `500 Internal Server Error`: Server-side error

### **Error Response Format**

```json
{
  "error": "Property not found: invalid.key",
  "status": 404
}
```

## Security Considerations

### **CORS Configuration**

- Currently allows all origins (`*`) for development
- Can be restricted to specific domains in production
- Supports preflight OPTIONS requests

### **Input Validation**

- All input is validated before processing
- JSON request bodies are parsed and validated
- Property keys are sanitized and validated

### **File System Access**

- Configuration files are read from predefined paths
- File system operations are restricted to configuration directories
- Automatic backup and validation of configuration changes

## Performance

### **Efficient Request Handling**

- Lightweight HTTP request processing
- Minimal memory overhead per request
- Fast JSON serialization/deserialization

### **Configuration Caching**

- Configuration properties are cached in memory
- File system access is minimized
- Background file monitoring with configurable intervals

## Monitoring and Debugging

### **Server Logs**

- Comprehensive logging of all API requests
- Configuration change tracking
- Error logging with stack traces

### **Health Checks**

- `/api/v1/config/status` endpoint for health monitoring
- Configuration validation status
- Property count and validation error reporting

### **Debug Information**

- Detailed error messages for troubleshooting
- Configuration file path and status information
- Property modification tracking

## Future Enhancements

### **Planned Features**

- **Authentication and Authorization**: User-based access control
- **Rate Limiting**: API request throttling
- **Metrics Collection**: Request/response statistics
- **WebSocket Support**: Real-time configuration updates
- **Configuration Templates**: Predefined configuration sets

### **API Extensions**

- **Batch Operations**: Update multiple properties at once
- **Configuration History**: Track configuration changes over time
- **Configuration Validation**: Schema-based validation rules
- **Configuration Export/Import**: Backup and restore functionality

## Troubleshooting

### **Common Issues**

#### **Server Won't Start**

- Check if port 8080 is available
- Verify configuration file exists and is valid
- Check server logs for error messages

#### **API Requests Fail**

- Ensure server is running on expected host/port
- Verify endpoint URLs are correct
- Check CORS settings for web applications

#### **Configuration Not Persisting**

- Verify file write permissions
- Check configuration file path
- Ensure `triggerSave()` is called after updates

#### **File Monitoring Not Working**

- Check if file monitoring is enabled
- Verify file system supports change detection
- Check monitoring thread status

### **Debug Commands**

```bash
# Check server status
curl http://localhost:8080/api/v1/config/status

# View server logs
tail -f server.log

# Test specific endpoint
curl -v http://localhost:8080/api/v1/config

# Check configuration file
cat config/config.yaml
```

## Contributing

When adding new API endpoints or modifying existing ones:

1. **Update the OpenAPI specification** in `OpenApiSpecHandler`
2. **Add appropriate error handling** for new endpoints
3. **Include unit tests** for new functionality
4. **Update this documentation** with new endpoint details
5. **Follow the existing code style** and patterns

## License

This web server component is part of the Media Deduplication Server project and follows the same licensing terms.
