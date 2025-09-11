#include "config/unified_observable_config.hpp"
#include <iostream>
#include <filesystem>

using namespace MediaDedup;

int main() {
    std::string test_file = "/tmp/debug_test2.yaml";
    
    // First manager (like in test)
    auto config_manager = std::make_unique<UnifiedObservableConfigManager>(
        test_file, false, std::chrono::milliseconds(100));
    
    if (!config_manager->initialize()) {
        std::cerr << "Failed to initialize config manager" << std::endl;
        return 1;
    }
    
    // Create properties
    config_manager->createProperty("persist.string", std::string("saved"), "Persistent string");
    config_manager->createProperty("persist.int", 999, "Persistent integer");
    
    std::cout << "Before save:" << std::endl;
    auto string_prop = config_manager->getProperty<std::string>("persist.string");
    auto int_prop = config_manager->getProperty<int>("persist.int");
    std::cout << "String: " << string_prop->getValueAs<std::string>() << std::endl;
    std::cout << "Int: " << int_prop->getValueAs<int>() << std::endl;
    
    // Save configuration
    if (!config_manager->triggerSave()) {
        std::cerr << "Failed to save configuration" << std::endl;
        return 1;
    }
    
    std::cout << "File contents after save:" << std::endl;
    if (std::filesystem::exists(test_file)) {
        std::ifstream file(test_file);
        std::string content((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
        std::cout << content << std::endl;
    }
    
    // Keep first manager alive (like in test)
    // config_manager->shutdown(); // Don't shutdown yet
    
    // Create new manager and load configuration
    auto new_manager = std::make_unique<UnifiedObservableConfigManager>(
        test_file, false, std::chrono::milliseconds(100));
    
    if (!new_manager->initialize()) {
        std::cerr << "Failed to initialize new config manager" << std::endl;
        return 1;
    }
    
    std::cout << "After load:" << std::endl;
    auto new_string_prop = new_manager->getProperty<std::string>("persist.string");
    auto new_int_prop = new_manager->getProperty<int>("persist.int");
    
    if (new_string_prop) {
        std::cout << "String: '" << new_string_prop->getValueAs<std::string>() << "'" << std::endl;
    } else {
        std::cout << "String property not found" << std::endl;
    }
    
    if (new_int_prop) {
        std::cout << "Int: " << new_int_prop->getValueAs<int>() << std::endl;
    } else {
        std::cout << "Int property not found" << std::endl;
    }
    
    return 0;
}
