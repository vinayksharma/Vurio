#include "config/observable_config_manager.hpp"
#include "config/log_level.hpp"
#include <iostream>
#include <thread>
#include <chrono>

using namespace MediaDedup;

/**
 * @brief Demo program showing bidirectional observable configuration
 *
 * This demo demonstrates:
 * 1. Loading configuration from file
 * 2. Setting properties programmatically (triggers file update)
 * 3. File changes triggering property updates
 * 4. Change callbacks and validation
 */
int main()
{
    std::cout << "=== Observable Configuration Demo ===\n\n";

    // Create configuration manager
    ObservableConfigManager config_manager("config/logging.yaml", true, std::chrono::milliseconds(500));

    // Set up callbacks
    config_manager.setFileChangeCallback([](const std::string &file_path)
                                         { std::cout << "📁 File changed: " << file_path << std::endl; });

    config_manager.setPropertyChangeCallback([](const std::string &key, const std::string &old_value, const std::string &new_value)
                                             { std::cout << "🔄 Property changed: " << key << " = " << old_value << " -> " << new_value << std::endl; });

    // Initialize and load configuration
    if (!config_manager.initialize())
    {
        std::cout << "❌ Failed to initialize configuration manager\n";
        return 1;
    }

    if (!config_manager.loadConfiguration())
    {
        std::cout << "❌ Failed to load configuration\n";
        return 1;
    }

    std::cout << "✅ Configuration loaded successfully\n\n";

    // Get the log level property
    auto log_level_prop = config_manager.getLogLevelProperty("log_level");
    if (log_level_prop)
    {
        std::cout << "📊 Current log level: " << log_level_prop->getValueAsString()
                  << " (" << log_level_prop->getCurrentLevelDescription() << ")\n";

        // Set up a change callback for this specific property
        log_level_prop->setChangeCallback([](const LogLevel &old_level, const LogLevel &new_level)
                                          {
            std::cout << "🎯 Log level changed from " << logLevelToString(old_level) 
                      << " to " << logLevelToString(new_level) << std::endl;
            
            // This is where you would update your logging system
            std::cout << "   Updating logging system...\n"; });

        std::cout << "👀 Watching for log level changes...\n\n";
    }

    // Demo 1: Programmatic changes trigger file updates
    std::cout << "=== Demo 1: Programmatic Changes ===\n";
    std::cout << "Setting log level to DEBUG programmatically...\n";

    if (config_manager.setPropertyValue("log_level", LogLevel::DEBUG))
    {
        std::cout << "✅ Log level set to DEBUG\n";
        std::cout << "📁 Configuration file should be updated automatically\n\n";
    }
    else
    {
        std::cout << "❌ Failed to set log level\n\n";
    }

    // Demo 2: Show current state
    std::cout << "=== Demo 2: Current State ===\n";
    auto current_level = config_manager.getPropertyValue("log_level", LogLevel::INFO);
    std::cout << "Current log level: " << logLevelToString(current_level) << "\n";

    // Check what levels are enabled
    if (log_level_prop)
    {
        std::cout << "TRACE enabled: " << (log_level_prop->isTraceEnabled() ? "Yes" : "No") << "\n";
        std::cout << "DEBUG enabled: " << (log_level_prop->isDebugEnabled() ? "Yes" : "No") << "\n";
        std::cout << "INFO enabled: " << (log_level_prop->isInfoEnabled() ? "Yes" : "No") << "\n";
        std::cout << "WARN enabled: " << (log_level_prop->isWarnEnabled() ? "Yes" : "No") << "\n";
        std::cout << "ERROR enabled: " << (log_level_prop->isErrorEnabled() ? "Yes" : "No") << "\n";
        std::cout << "FATAL enabled: " << (log_level_prop->isFatalEnabled() ? "Yes" : "No") << "\n\n";
    }

    // Demo 3: Level manipulation
    std::cout << "=== Demo 3: Level Manipulation ===\n";
    if (log_level_prop)
    {
        std::cout << "Increasing log level (making more verbose)...\n";
        if (log_level_prop->increaseLevel())
        {
            std::cout << "✅ Log level increased to: " << log_level_prop->getValueAsString() << "\n";
        }
        else
        {
            std::cout << "❌ Cannot increase level (already at most verbose)\n";
        }

        std::cout << "Setting to most verbose...\n";
        log_level_prop->setMostVerbose();
        std::cout << "✅ Log level set to: " << log_level_prop->getValueAsString() << "\n\n";
    }

    // Demo 4: Show all available options
    std::cout << "=== Demo 4: Available Options ===\n";
    auto available_levels = ObservableLogLevel::getAvailableOptions();
    std::cout << "Available log levels:\n";
    for (const auto &level_str : available_levels)
    {
        LogLevel level = stringToLogLevel(level_str);
        std::cout << "  - " << level_str << ": " << getLogLevelDescription(level) << "\n";
    }
    std::cout << "\n";

    // Demo 5: File monitoring
    std::cout << "=== Demo 5: File Monitoring ===\n";
    std::cout << "The configuration manager is now monitoring the file for changes.\n";
    std::cout << "Try editing config/logging.yaml in another terminal and watch for updates!\n\n";

    std::cout << "Monitoring for 30 seconds...\n";
    std::cout << "Press Ctrl+C to stop early\n\n";

    // Monitor for a while
    try
    {
        for (int i = 0; i < 30; ++i)
        {
            std::cout << "\r⏱️  Monitoring... " << (30 - i) << "s remaining" << std::flush;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        std::cout << "\n";
    }
    catch (...)
    {
        std::cout << "\n⏹️  Monitoring stopped by user\n";
    }

    std::cout << "\n=== Demo Complete ===\n";
    std::cout << "Configuration file: " << config_manager.getConfigFilePath() << "\n";
    std::cout << "Final log level: " << logLevelToString(config_manager.getPropertyValue("log_level", LogLevel::INFO)) << "\n";

    return 0;
}
