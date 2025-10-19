#include "config/unified_observable_config.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <iomanip>

using namespace MediaDedup;

// Helper function to print configuration events
void printConfigEvent(const ConfigChangeEvent &event)
{
    auto time_t = std::chrono::system_clock::to_time_t(event.timestamp);
    auto tm = *std::localtime(&time_t);

    std::cout << std::put_time(&tm, "%H:%M:%S") << " [CONFIG] "
              << event.key << " changed from ";

    // Print old value
    if (event.old_value.type() == typeid(std::string))
    {
        std::cout << "'" << std::any_cast<std::string>(event.old_value) << "'";
    }
    else if (event.old_value.type() == typeid(int))
    {
        std::cout << std::any_cast<int>(event.old_value);
    }
    else if (event.old_value.type() == typeid(bool))
    {
        std::cout << (std::any_cast<bool>(event.old_value) ? "true" : "false");
    }
    else
    {
        std::cout << "<unknown>";
    }

    std::cout << " to ";

    // Print new value
    if (event.new_value.type() == typeid(std::string))
    {
        std::cout << "'" << std::any_cast<std::string>(event.new_value) << "'";
    }
    else if (event.new_value.type() == typeid(int))
    {
        std::cout << std::any_cast<int>(event.new_value);
    }
    else if (event.new_value.type() == typeid(bool))
    {
        std::cout << (std::any_cast<bool>(event.new_value) ? "true" : "false");
    }
    else
    {
        std::cout << "<unknown>";
    }

    std::cout << " (source: " << event.source
              << ", file_update: " << (event.is_file_update ? "yes" : "no") << ")"
              << std::endl;
}

// Helper function to print file change events
void printFileChange(const std::string &file_path)
{
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto tm = *std::localtime(&time_t);

    std::cout << std::put_time(&tm, "%H:%M:%S") << " [FILE] "
              << "Configuration file changed: " << file_path << std::endl;
}

int main()
{
    std::cout << "=== Unified Observable Configuration Demo ===" << std::endl;
    std::cout << "This demo shows the unified configuration system in action." << std::endl;
    std::cout << std::endl;

    // Create configuration manager
    UnifiedObservableConfigManager config("config/unified_demo.yaml", true, std::chrono::milliseconds(500));

    // Subscribe to configuration changes
    config.subscribeToConfigChanges(printConfigEvent);

    // Subscribe to file changes
    config.setFileChangeCallback(printFileChange);

    // Initialize the configuration
    if (!config.initialize())
    {
        std::cerr << "Failed to initialize configuration manager" << std::endl;
        return 1;
    }

    std::cout << "Configuration manager initialized successfully" << std::endl;
    std::cout << std::endl;

    // Create configuration properties with different types
    std::cout << "Creating configuration properties..." << std::endl;

    auto server_host = config.createProperty("server.host", std::string("localhost"), "Server host address");
    auto server_port = config.createProperty("server.port", 8080, "Server port number");
    auto enable_debug = config.createProperty("debug.enabled", false, "Enable debug mode");
    auto log_level = config.createProperty("logging.level", std::string("info"), "Log level");
    auto max_connections = config.createProperty("server.max_connections", 100, "Maximum connections");
    auto timeout = config.createProperty("server.timeout", 30.0, "Request timeout in seconds");

    // Add validation for port number
    server_port->setValidationCallback([](const std::any &value)
                                       {
        try {
            int port = std::any_cast<int>(value);
            return port > 0 && port < 65536;
        } catch (...) {
            return false;
        } });

    // Add validation for log level
    log_level->setValidationCallback([](const std::any &value)
                                     {
        try {
            std::string level = std::any_cast<std::string>(value);
            std::transform(level.begin(), level.end(), level.begin(), ::tolower);
            return level == "trace" || level == "debug" || level == "info" || 
                   level == "warn" || level == "error" || level == "fatal";
        } catch (...) {
            return false;
        } });

    std::cout << "Properties created. Current configuration:" << std::endl;
    std::cout << "  server.host: " << server_host->getValueAs<std::string>() << std::endl;
    std::cout << "  server.port: " << server_port->getValueAs<int>() << std::endl;
    std::cout << "  debug.enabled: " << (enable_debug->getValueAs<bool>() ? "true" : "false") << std::endl;
    std::cout << "  logging.level: " << log_level->getValueAs<std::string>() << std::endl;
    std::cout << "  server.max_connections: " << max_connections->getValueAs<int>() << std::endl;
    std::cout << "  server.timeout: " << timeout->getValueAs<double>() << std::endl;
    std::cout << std::endl;

    // Demonstrate programmatic changes
    std::cout << "=== Demonstrating Programmatic Changes ===" << std::endl;

    std::cout << "Changing server port to 9090..." << std::endl;
    if (config.setPropertyValue("server.port", 9090))
    {
        std::cout << "  Port changed successfully" << std::endl;
    }
    else
    {
        std::cout << "  Failed to change port" << std::endl;
    }

    std::cout << "Changing log level to debug..." << std::endl;
    if (config.setPropertyValue("logging.level", std::string("debug")))
    {
        std::cout << "  Log level changed successfully" << std::endl;
    }
    else
    {
        std::cout << "  Failed to change log level" << std::endl;
    }

    std::cout << "Enabling debug mode..." << std::endl;
    if (config.setPropertyValue("debug.enabled", true))
    {
        std::cout << "  Debug mode enabled" << std::endl;
    }
    else
    {
        std::cout << "  Failed to enable debug mode" << std::endl;
    }

    std::cout << std::endl;

    // Demonstrate validation
    std::cout << "=== Demonstrating Validation ===" << std::endl;

    std::cout << "Trying to set invalid port (70000)..." << std::endl;
    if (config.setPropertyValue("server.port", 70000))
    {
        std::cout << "  Port changed (validation failed)" << std::endl;
    }
    else
    {
        std::cout << "  Port change rejected (validation working)" << std::endl;
    }

    std::cout << "Trying to set invalid log level (invalid_level)..." << std::endl;
    if (config.setPropertyValue("logging.level", std::string("invalid_level")))
    {
        std::cout << "  Log level changed (validation failed)" << std::endl;
    }
    else
    {
        std::cout << "  Log level change rejected (validation working)" << std::endl;
    }

    std::cout << std::endl;

    // Demonstrate reset to defaults
    std::cout << "=== Demonstrating Reset to Defaults ===" << std::endl;

    std::cout << "Resetting server.port to default..." << std::endl;
    server_port->resetToDefault();

    std::cout << "Resetting all properties to defaults..." << std::endl;
    config.resetToDefaults();

    std::cout << std::endl;

    // Show current configuration after reset
    std::cout << "Configuration after reset:" << std::endl;
    std::cout << "  server.host: " << server_host->getValueAs<std::string>() << std::endl;
    std::cout << "  server.port: " << server_port->getValueAs<int>() << std::endl;
    std::cout << "  debug.enabled: " << (enable_debug->getValueAs<bool>() ? "true" : "false") << std::endl;
    std::cout << "  logging.level: " << log_level->getValueAs<std::string>() << std::endl;
    std::cout << std::endl;

    // Demonstrate string conversion
    std::cout << "=== Demonstrating String Conversion ===" << std::endl;

    std::cout << "Setting values via string conversion..." << std::endl;

    if (server_port->setValueFromString("1234"))
    {
        std::cout << "  Port set to 1234 via string" << std::endl;
    }

    if (enable_debug->setValueFromString("true"))
    {
        std::cout << "  Debug enabled via string" << std::endl;
    }

    if (timeout->setValueFromString("60.5"))
    {
        std::cout << "  Timeout set to 60.5 via string" << std::endl;
    }

    std::cout << std::endl;

    // Show string representations
    std::cout << "String representations:" << std::endl;
    std::cout << "  server.port: '" << server_port->getValueAsString() << "'" << std::endl;
    std::cout << "  debug.enabled: '" << enable_debug->getValueAsString() << "'" << std::endl;
    std::cout << "  server.timeout: '" << timeout->getValueAsString() << "'" << std::endl;
    std::cout << std::endl;

    // Demonstrate file monitoring
    std::cout << "=== File Monitoring Active ===" << std::endl;
    std::cout << "The configuration file is being monitored for changes." << std::endl;
    std::cout << "You can edit 'config/unified_demo.yaml' in another terminal" << std::endl;
    std::cout << "to see file change events in real-time." << std::endl;
    std::cout << std::endl;

    // Show configuration status
    std::cout << "=== Configuration Status ===" << std::endl;
    std::cout << config.toString() << std::endl;

    // Keep the program running to demonstrate file monitoring
    std::cout << "=== Demo Running ===" << std::endl;
    std::cout << "Press Ctrl+C to exit. The configuration file will be saved automatically." << std::endl;
    std::cout << std::endl;

    try
    {
        // Keep running to demonstrate file monitoring
        while (true)
        {
            std::this_thread::sleep_for(std::chrono::seconds(5));

            // Show some live stats
            auto keys = config.getAllPropertyKeys();
            std::cout << "Active properties: " << keys.size() << std::endl;
        }
    }
    catch (const std::exception &e)
    {
        std::cout << "Demo interrupted: " << e.what() << std::endl;
    }

    // Shutdown
    config.shutdown();
    std::cout << "Configuration manager shut down successfully" << std::endl;

    return 0;
}
