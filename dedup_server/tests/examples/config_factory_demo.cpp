#include "config/config_manager_factory.hpp"
#include <iostream>
#include <thread>
#include <chrono>

int main()
{
    std::cout << "=== Config Manager Factory Demo ===" << std::endl;
    std::cout << "This demo shows how to use the ConfigManagerFactory to create configured instances." << std::endl;
    std::cout << std::endl;

    try
    {
        // Demo 1: Create default configuration manager
        std::cout << "=== Demo 1: Default Configuration Manager ===" << std::endl;
        auto default_manager = MediaDedup::ConfigManagerFactory::createDefault("config/factory_demo.yaml");
        std::cout << "Default manager created successfully" << std::endl;
        std::cout << "Properties count: " << default_manager->getPropertyCount() << std::endl;
        std::cout << "File monitoring enabled: " << (default_manager->isFileMonitoringEnabled() ? "yes" : "no") << std::endl;
        std::cout << "Validation enabled: " << (default_manager->isValidationEnabled() ? "yes" : "no") << std::endl;
        std::cout << std::endl;

        // Demo 2: Create testing configuration manager
        std::cout << "=== Demo 2: Testing Configuration Manager ===" << std::endl;
        auto test_manager = MediaDedup::ConfigManagerFactory::createForTesting("config/test_factory_demo.yaml");
        std::cout << "Test manager created successfully" << std::endl;
        std::cout << "Properties count: " << test_manager->getPropertyCount() << std::endl;
        std::cout << "File monitoring enabled: " << (test_manager->isFileMonitoringEnabled() ? "yes" : "no") << std::endl;
        std::cout << "Validation enabled: " << (test_manager->isValidationEnabled() ? "yes" : "no") << std::endl;
        std::cout << std::endl;

        // Demo 3: Create development configuration manager
        std::cout << "=== Demo 3: Development Configuration Manager ===" << std::endl;
        auto dev_manager = MediaDedup::ConfigManagerFactory::createForDevelopment("config/dev_factory_demo.yaml");
        std::cout << "Development manager created successfully" << std::endl;
        std::cout << "Properties count: " << dev_manager->getPropertyCount() << std::endl;
        std::cout << "File monitoring enabled: " << (dev_manager->isFileMonitoringEnabled() ? "yes" : "no") << std::endl;
        std::cout << "Validation enabled: " << (dev_manager->isValidationEnabled() ? "yes" : "no") << std::endl;
        std::cout << "Reload interval: " << dev_manager->getReloadInterval().count() << "ms" << std::endl;
        std::cout << std::endl;

        // Demo 4: Create production configuration manager
        std::cout << "=== Demo 4: Production Configuration Manager ===" << std::endl;
        auto prod_manager = MediaDedup::ConfigManagerFactory::createForProduction("config/prod_factory_demo.yaml", true);
        std::cout << "Production manager created successfully" << std::endl;
        std::cout << "Properties count: " << prod_manager->getPropertyCount() << std::endl;
        std::cout << "File monitoring enabled: " << (prod_manager->isFileMonitoringEnabled() ? "yes" : "no") << std::endl;
        std::cout << "Validation enabled: " << (prod_manager->isValidationEnabled() ? "yes" : "no") << std::endl;
        std::cout << "Reload interval: " << prod_manager->getReloadInterval().count() << "ms" << std::endl;
        std::cout << std::endl;

        // Demo 5: Create custom configuration manager
        std::cout << "=== Demo 5: Custom Configuration Manager ===" << std::endl;
        MediaDedup::ConfigManagerConfig custom_config;
        custom_config.config_file_path = "config/custom_factory_demo.yaml";
        custom_config.enable_file_monitoring = true;
        custom_config.enable_validation = true;
        custom_config.auto_save = true;
        custom_config.enable_events = true;
        custom_config.emit_file_change_events = true;
        custom_config.emit_programmatic_events = true;
        custom_config.strict_validation = false;
        custom_config.validate_on_load = true;
        custom_config.validate_on_save = true;
        custom_config.reload_interval = std::chrono::milliseconds(1500);
        custom_config.file_check_interval = std::chrono::milliseconds(750);
        custom_config.log_level = "debug";

        auto custom_manager = MediaDedup::ConfigManagerFactory::createWithConfig(custom_config);
        std::cout << "Custom manager created successfully" << std::endl;
        std::cout << "Properties count: " << custom_manager->getPropertyCount() << std::endl;
        std::cout << "File monitoring enabled: " << (custom_manager->isFileMonitoringEnabled() ? "yes" : "no") << std::endl;
        std::cout << "Validation enabled: " << (custom_manager->isValidationEnabled() ? "yes" : "no") << std::endl;
        std::cout << "Reload interval: " << custom_manager->getReloadInterval().count() << "ms" << std::endl;
        std::cout << std::endl;

        // Demo 6: Test property operations
        std::cout << "=== Demo 6: Property Operations ===" << std::endl;
        std::cout << "Setting server port to 9090..." << std::endl;
        if (custom_manager->setPropertyValue("server.port", 9090))
        {
            std::cout << "  Port set successfully" << std::endl;
        }
        else
        {
            std::cout << "  Failed to set port" << std::endl;
        }

        std::cout << "Setting log level to debug..." << std::endl;
        if (custom_manager->setPropertyValue("logging.level", std::string("debug")))
        {
            std::cout << "  Log level set successfully" << std::endl;
        }
        else
        {
            std::cout << "  Failed to set log level" << std::endl;
        }

        std::cout << "Enabling debug mode..." << std::endl;
        if (custom_manager->setPropertyValue("debug.enabled", true))
        {
            std::cout << "  Debug mode enabled successfully" << std::endl;
        }
        else
        {
            std::cout << "  Failed to enable debug mode" << std::endl;
        }

        // Demo 7: Test validation
        std::cout << "=== Demo 7: Validation Testing ===" << std::endl;
        std::cout << "Trying to set invalid port (70000)..." << std::endl;
        if (custom_manager->setPropertyValue("server.port", 70000))
        {
            std::cout << "  Port set successfully (unexpected!)" << std::endl;
        }
        else
        {
            std::cout << "  Port change rejected (validation working)" << std::endl;
        }

        std::cout << "Trying to set invalid log level..." << std::endl;
        if (custom_manager->setPropertyValue("logging.level", std::string("invalid_level")))
        {
            std::cout << "  Log level set successfully (unexpected!)" << std::endl;
        }
        else
        {
            std::cout << "  Log level change rejected (validation working)" << std::endl;
        }

        // Demo 8: Show configuration status
        std::cout << "=== Demo 8: Configuration Status ===" << std::endl;
        std::cout << custom_manager->toString() << std::endl;

        // Demo 9: Test environment-based creation
        std::cout << "=== Demo 9: Environment-based Creation ===" << std::endl;
        std::cout << "Setting environment variables..." << std::endl;
        setenv("CONFIG_ENABLE_MONITORING", "true", 1);
        setenv("CONFIG_ENABLE_VALIDATION", "true", 1);
        setenv("CONFIG_AUTO_SAVE", "true", 1);
        setenv("CONFIG_STRICT_VALIDATION", "false", 1);
        setenv("CONFIG_RELOAD_INTERVAL", "3000", 1);
        setenv("CONFIG_LOG_LEVEL", "info", 1);

        auto env_manager = MediaDedup::ConfigManagerFactory::createFromEnvironment("config/env_factory_demo.yaml");
        std::cout << "Environment-based manager created successfully" << std::endl;
        std::cout << "Properties count: " << env_manager->getPropertyCount() << std::endl;
        std::cout << "File monitoring enabled: " << (env_manager->isFileMonitoringEnabled() ? "yes" : "no") << std::endl;
        std::cout << "Validation enabled: " << (env_manager->isValidationEnabled() ? "yes" : "no") << std::endl;
        std::cout << "Reload interval: " << env_manager->getReloadInterval().count() << "ms" << std::endl;
        std::cout << std::endl;

        std::cout << "=== Demo Complete ===" << std::endl;
        std::cout << "All configuration managers created and tested successfully!" << std::endl;
        std::cout << "The factory pattern provides clean, configurable instances for different use cases." << std::endl;

        // Clean up
        default_manager->shutdown();
        test_manager->shutdown();
        dev_manager->shutdown();
        prod_manager->shutdown();
        custom_manager->shutdown();
        env_manager->shutdown();
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
