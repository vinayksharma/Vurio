#include <iostream>
#include "include/core/server_config_manager.hpp"
#include "include/core/dedup_modes.hpp"
#include "include/logging/logger.hpp"

int main()
{
    // Initialize logger
    Logger::init();

    // Get the configuration manager instance
    auto &config_manager = ServerConfigManager::getInstance();

    // Get the current mode
    auto mode = config_manager.getDedupMode();
    auto mode_name = DedupModes::getModeName(mode);

    std::cout << "Current dedup mode: " << mode_name << std::endl;

    // Get the raw config
    auto config = config_manager.getConfig();
    std::cout << "Raw config dedup_mode: " << config["dedup_mode"] << std::endl;

    // Check if config.json exists and read it
    std::ifstream file("config.json");
    if (file.is_open())
    {
        std::string content((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
        std::cout << "config.json content:" << std::endl
                  << content << std::endl;
    }
    else
    {
        std::cout << "config.json not found" << std::endl;
    }

    return 0;
}