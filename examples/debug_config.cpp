#include <iostream>
#include "include/core/server_config_manager.hpp"
#include "include/core/dedup_modes.hpp"
#include "include/logging/logger.hpp"

int main()
{
    // Initialize logger
    Logger::init();

    // Get current dedup mode
    auto &config_manager = ServerConfigManager::getInstance();
    auto current_mode = config_manager.getDedupMode();
    std::string mode_name = DedupModes::getModeName(current_mode);
    Logger::info("Current dedup mode: " + mode_name);

    // Check raw config
    YAML::Node config = config_manager.getConfig();
    Logger::info("Raw config dedup_mode: " + config["dedup_mode"].as<std::string>());

    // Try to read config.json if it exists
    std::ifstream config_file("config.json");
    if (config_file.is_open())
    {
        Logger::info("config.json content:");
        std::string content((std::istreambuf_iterator<char>(config_file)),
                            std::istreambuf_iterator<char>());
        Logger::info(content);
        config_file.close();
    }
    else
    {
        Logger::info("config.json not found");
    }

    return 0;
}