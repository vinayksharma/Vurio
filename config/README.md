# Observable Configuration System

This directory contains a sophisticated configuration management system that provides **bidirectional observable properties** for your media deduplication server.

## 🎯 **What This System Provides**

### **Bidirectional Updates**

- **Programmatic changes** → **File updates** automatically
- **File changes** → **Property updates** automatically
- **Real-time synchronization** between code and configuration files

### **Observable Properties**

- **Watch for changes** in configuration values
- **Execute callbacks** when properties change
- **Validate values** before applying changes
- **Thread-safe operations** with proper locking

### **Log Level Management**

- **Specialized LogLevel enum** with validation
- **String conversion** for YAML/JSON files
- **Level manipulation** (increase/decrease, set to extremes)
- **Level checking** (isTraceEnabled, isDebugEnabled, etc.)

## 🏗️ **Architecture Overview**

```
┌─────────────────┐    ┌──────────────────┐    ┌─────────────────┐
│   Code/App      │◄──►│ Observable Props │◄──►│  Config Files   │
│                 │    │                  │    │                 │
│ setLogLevel()   │    │ Change Callbacks │    │ YAML/JSON      │
│ getLogLevel()   │    │ Validation       │    │ Auto-reload    │
└─────────────────┘    └──────────────────┘    └─────────────────┘
```

## 📁 **File Structure**

```
config/
├── logging.yaml                    # Logging configuration file
├── observable_property.hpp         # Generic observable property template
├── log_level.hpp                   # Log level enum and utilities
├── observable_log_level.hpp        # Specialized log level property
├── observable_config_manager.hpp   # Main configuration manager
├── examples/
│   └── config_demo.cpp            # Demo program
└── README.md                       # This file
```

## 🚀 **Quick Start**

### **1. Basic Usage**

```cpp
#include "config/observable_config_manager.hpp"
#include "config/log_level.hpp"

using namespace MediaDedup;

// Create configuration manager
ObservableConfigManager config("config/logging.yaml");

// Initialize and load
config.initialize();
config.loadConfiguration();

// Get log level property
auto log_level = config.getLogLevelProperty("log_level");
if (log_level) {
    // Set up change callback
    log_level->setChangeCallback([](const LogLevel& old_level, const LogLevel& new_level) {
        std::cout << "Log level changed from " << logLevelToString(old_level)
                  << " to " << logLevelToString(new_level) << std::endl;
        // Update your logging system here
    });

    // Change log level programmatically
    log_level->setValue(LogLevel::DEBUG);
    // This automatically updates the config file!
}
```

### **2. File Monitoring**

```cpp
// Enable automatic file reloading
config.setAutoReload(true);
config.setReloadInterval(std::chrono::milliseconds(1000));

// Set up file change callback
config.setFileChangeCallback([](const std::string& file_path) {
    std::cout << "Configuration file changed: " << file_path << std::endl;
});

// Set up property change callback
config.setPropertyChangeCallback([](const std::string& key,
                                   const std::string& old_value,
                                   const std::string& new_value) {
    std::cout << "Property " << key << " changed: "
              << old_value << " -> " << new_value << std::endl;
});
```

### **3. Log Level Operations**

```cpp
auto log_level = config.getLogLevelProperty("log_level");

// Check if specific levels are enabled
if (log_level->isDebugEnabled()) {
    std::cout << "Debug logging is enabled\n";
}

// Manipulate log levels
log_level->increaseLevel();        // Make more verbose
log_level->decreaseLevel();        // Make less verbose
log_level->setMostVerbose();       // Set to TRACE
log_level->setLeastVerbose();      // Set to FATAL only

// Get level information
std::cout << "Current level: " << log_level->getValueAsString() << "\n";
std::cout << "Description: " << log_level->getCurrentLevelDescription() << "\n";
```

## 📊 **Configuration File Format**

The system supports YAML configuration files. Here's an example:

```yaml
# Log Level Configuration
log_level: "info" # trace, debug, info, warn, error, fatal

# Log Output Configuration
log_output:
  console: true
  file: true
  syslog: false

# File Logging Configuration
file_logging:
  path: "logs/media_dedup.log"
  max_size: 10485760
  max_files: 5
  rotation: "daily"
  compress: true
```

## 🔧 **Advanced Features**

### **Validation Callbacks**

```cpp
auto property = config.getProperty<bool>("enable_debug");
if (property) {
    property->setValidationCallback([](const bool& value) -> bool {
        // Only allow debug in development mode
        return !isProductionEnvironment() || !value;
    });
}
```

### **Custom Property Types**

```cpp
// Create custom observable property
auto custom_prop = std::make_shared<ObservableProperty<std::string>>(
    "custom_setting", "default_value", "Custom configuration setting"
);

// Set up callbacks
custom_prop->setChangeCallback([](const std::string& old_val, const std::string& new_val) {
    std::cout << "Custom setting changed: " << old_val << " -> " << new_val << "\n";
});
```

### **Thread Safety**

All operations are thread-safe with proper mutex locking:

```cpp
// Multiple threads can safely access properties
std::thread t1([&config]() {
    config.setPropertyValue("log_level", LogLevel::DEBUG);
});

std::thread t2([&config]() {
    auto level = config.getPropertyValue("log_level", LogLevel::INFO);
    std::cout << "Thread 2 sees level: " << logLevelToString(level) << "\n";
});

t1.join();
t2.join();
```

## 🧪 **Running the Demo**

```bash
# Build the demo
cd examples
g++ -std=c++17 -I../.. config_demo.cpp -o config_demo

# Run the demo
./config_demo
```

The demo will:

1. Load configuration from `config/logging.yaml`
2. Set up change callbacks
3. Demonstrate programmatic changes
4. Monitor for file changes
5. Show bidirectional synchronization

## ⚠️ **Important Notes**

### **File Permissions**

- Ensure the configuration file is writable by your application
- The system will automatically create backup files if needed

### **File Format**

- Currently supports YAML format
- File must be valid YAML syntax
- Invalid files will trigger validation errors

### **Performance**

- File monitoring runs in a separate thread
- Default reload interval is 1 second
- Adjust based on your needs

### **Error Handling**

- Always check return values from initialization methods
- Use `isValid()` to check configuration validity
- Check `getValidationErrors()` for detailed error information

## 🔮 **Future Enhancements**

- **JSON configuration** support
- **Environment variable** overrides
- **Configuration encryption** for sensitive data
- **Remote configuration** via HTTP/HTTPS
- **Configuration validation** schemas
- **Hot reloading** without restart

## 🤝 **Contributing**

When adding new configuration properties:

1. **Use appropriate types** (string, int, bool, double, LogLevel)
2. **Add validation** where appropriate
3. **Document the property** in configuration files
4. **Add to demo** if it's a key feature
5. **Test bidirectional updates** thoroughly

## 📚 **API Reference**

See the header files for complete API documentation:

- `ObservableProperty<T>` - Generic observable property
- `ObservableLogLevel` - Specialized log level property
- `ObservableConfigManager` - Main configuration manager
- `LogLevel` - Log level enumeration

---

**This system provides a robust, flexible foundation for configuration management in your media deduplication server! 🎉**
