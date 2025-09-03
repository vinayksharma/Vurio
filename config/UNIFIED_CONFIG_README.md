# Unified Observable Configuration System

## Overview

The Unified Observable Configuration System provides a generic, event-driven approach to configuration management that eliminates the need for dedicated observable classes for each configuration type. Instead, it uses a unified key-value system with comprehensive event handling and bidirectional synchronization.

## Key Features

### 🔄 **Unified Event System**

- Single event structure (`ConfigChangeEvent`) for all configuration changes
- Generic property wrapper (`ObservableConfigProperty`) that works with any data type
- Event-driven architecture with subscriber pattern

### 🚫 **No Circular References**

- Smart event filtering prevents infinite loops
- File updates are marked and don't trigger programmatic events
- Clean separation between file and programmatic changes

### 🎯 **Generic Type Support**

- Uses `std::any` for type-agnostic property storage
- Automatic type conversion and validation
- Support for strings, integers, booleans, doubles, and vectors

### 📁 **Bidirectional File Sync**

- Programmatic changes automatically update configuration files
- File changes automatically update in-memory properties
- Real-time file monitoring with configurable intervals

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                 UnifiedObservableConfigManager              │
├─────────────────────────────────────────────────────────────┤
│  Properties Storage    │  Event System    │  File Monitor  │
│  ┌─────────────────┐  │  ┌─────────────┐  │  ┌──────────┐  │
│  │ ObservableConfig│  │  │ Subscribers │  │  │ File     │  │
│  │ Property 1      │  │  │ Callbacks   │  │  │ Watcher  │  │
│  │ ObservableConfig│  │  │ Event Queue │  │  │ Thread   │  │
│  │ Property 2      │  │  └─────────────┘  │  └──────────┘  │
│  │ ...             │  │                   │                │
│  └─────────────────┘  │                   │                │
└─────────────────────────────────────────────────────────────┘
                                │
                                ▼
                    ┌─────────────────────┐
                    │   YAML/JSON Files  │
                    │   (Auto-synced)    │
                    └─────────────────────┘
```

## Core Components

### 1. ConfigChangeEvent

```cpp
struct ConfigChangeEvent {
    std::string key;                    // Configuration key
    std::any old_value;                 // Previous value
    std::any new_value;                 // New value
    std::string source;                 // Change source
    std::chrono::system_clock::time_point timestamp;
    bool is_file_update;                 // Prevents circular refs
};
```

### 2. ObservableConfigProperty

```cpp
class ObservableConfigProperty {
    // Generic property wrapper with type safety
    template<typename T>
    T getValueAs() const;

    bool setValue(const std::any& value, const std::string& source = "programmatic");
    bool setValueFromFile(const std::any& value);

    // Validation and callbacks
    void setValidationCallback(ValidationCallback callback);
    void setChangeCallback(ChangeCallback callback);
};
```

### 3. UnifiedObservableConfigManager

```cpp
class UnifiedObservableConfigManager {
    // Property management
    template<typename T>
    std::shared_ptr<ObservableConfigProperty> createProperty(
        const std::string& key, const T& default_value, const std::string& description);

    // Event system
    void subscribeToConfigChanges(ConfigChangeCallback callback);
    void emitConfigChangeEvent(const ConfigChangeEvent& event);

    // File operations
    bool loadConfiguration();
    bool saveConfiguration();
    void setAutoReload(bool enable);
};
```

## Usage Examples

### Basic Property Creation

```cpp
// Create configuration manager
UnifiedObservableConfigManager config("config/app.yaml");

// Create properties of different types
auto server_host = config.createProperty("server.host", std::string("localhost"));
auto server_port = config.createProperty("server.port", 8080);
auto debug_mode = config.createProperty("debug.enabled", false);

// Subscribe to changes
config.subscribeToConfigChanges([](const ConfigChangeEvent& event) {
    std::cout << event.key << " changed from "
              << event.old_value << " to " << event.new_value << std::endl;
});
```

### Validation and Type Safety

```cpp
auto port_prop = config.createProperty("server.port", 8080);

// Add validation
port_prop->setValidationCallback([](const std::any& value) {
    try {
        int port = std::any_cast<int>(value);
        return port > 0 && port < 65536;
    } catch (...) {
        return false;
    }
});

// Invalid values will be rejected
config.setPropertyValue("server.port", 70000);  // Returns false
```

### File Monitoring

```cpp
// Enable automatic file monitoring
config.setAutoReload(true);
config.setReloadInterval(std::chrono::milliseconds(1000));

// Subscribe to file changes
config.setFileChangeCallback([](const std::string& file_path) {
    std::cout << "Configuration file changed: " << file_path << std::endl;
});
```

### String Conversion

```cpp
auto timeout_prop = config.createProperty("server.timeout", 30.0);

// Set via string (useful for command-line or file parsing)
timeout_prop->setValueFromString("60.5");

// Get string representation
std::string timeout_str = timeout_prop->getValueAsString();  // "60.500000"
```

## Configuration File Format

The system automatically generates and maintains YAML configuration files:

```yaml
# Auto-generated by UnifiedObservableConfigManager
server:
  host: localhost
  port: 8080
  timeout: 30.0
  max_connections: 100

debug:
  enabled: false

logging:
  level: info
  output: console
```

## Event Flow

### Programmatic Change Flow

```
1. Property.setValue() called
2. Validation executed
3. Value updated
4. ConfigChangeEvent created
5. Event emitted to subscribers
6. Configuration file updated
7. File modification time updated
```

### File Change Flow

```
1. File modification detected
2. YAML file parsed
3. Properties updated via setValueFromFile()
4. Events marked as file_update=true
5. Subscribers notified (no circular refs)
6. File modification time updated
```

## Benefits Over Previous System

### ✅ **Eliminated Issues**

- No more dedicated classes for each configuration type
- No circular reference problems
- Unified event handling
- Generic type support

### 🚀 **New Capabilities**

- Automatic file synchronization
- Real-time file monitoring
- Comprehensive validation system
- Thread-safe operations
- Performance optimizations

### 🔧 **Developer Experience**

- Single API for all configuration needs
- Template-based type safety
- Automatic serialization/deserialization
- Comprehensive testing framework

## Testing

The system includes comprehensive unit tests covering:

- Property creation and retrieval
- Value changes and events
- Validation callbacks
- String conversion
- Configuration persistence
- Concurrent access
- File monitoring

Run tests with:

```bash
./tests/run_tests.sh unit test_unified_observable_config
```

## Performance Considerations

- **Memory**: Uses `std::any` for type storage (minimal overhead)
- **CPU**: Efficient event filtering prevents unnecessary processing
- **I/O**: Configurable file monitoring intervals
- **Threading**: Lock-free event emission, minimal contention

## Migration from Old System

### Before (Old System)

```cpp
// Required separate classes for each type
ObservableLogLevel log_level;
ObservableProperty<std::string> server_host;
ObservableProperty<int> server_port;

// Separate event handling
log_level.setChangeCallback(logLevelCallback);
server_host.setChangeCallback(hostCallback);
```

### After (Unified System)

```cpp
// Single manager handles all types
UnifiedObservableConfigManager config;

// Generic property creation
auto log_level = config.createProperty("logging.level", std::string("info"));
auto server_host = config.createProperty("server.host", std::string("localhost"));
auto server_port = config.createProperty("server.port", 8080);

// Single event subscription
config.subscribeToConfigChanges(unifiedCallback);
```

## Future Enhancements

- **Nested Configuration**: Support for hierarchical property structures
- **Schema Validation**: JSON Schema or similar validation
- **Hot Reloading**: Runtime configuration updates without restart
- **Configuration Versioning**: Track configuration changes over time
- **Remote Configuration**: Support for remote configuration sources
- **Encryption**: Secure storage of sensitive configuration values

## Conclusion

The Unified Observable Configuration System provides a clean, efficient, and maintainable approach to configuration management. By eliminating the need for dedicated observable classes and implementing smart event filtering, it solves the circular reference problem while providing a more powerful and flexible configuration system.

This system is designed to scale with your application's needs and provides a solid foundation for future configuration management requirements.
