#pragma once

#include <string>
#include <memory>
#include <chrono>
#include <functional>
#include "config/unified_observable_config.hpp"

namespace MediaDedup
{

    /**
     * @brief Configuration for creating UnifiedObservableConfigManager instances
     */
    struct ConfigManagerConfig
    {
        std::string config_file_path;
        bool enable_file_monitoring = true;
        std::chrono::milliseconds reload_interval = std::chrono::milliseconds(1000);
        bool enable_validation = true;
        std::string log_level = "info";

        // File monitoring settings
        std::chrono::milliseconds file_check_interval = std::chrono::milliseconds(500);

        // Validation settings
        bool strict_validation = false;

        // Event system settings
        bool enable_events = true;
        bool emit_file_change_events = true;
        bool emit_programmatic_events = true;
    };

    /**
     * @brief Factory for creating configured UnifiedObservableConfigManager instances
     *
     * Provides a clean factory pattern for creating configuration managers with
     * proper dependency injection and component configuration.
     */
    class ConfigManagerFactory
    {
    public:

        /**
         * @brief Create a configuration manager with custom configuration
         * @param config Configuration settings
         * @return Shared pointer to configured UnifiedObservableConfigManager
         */
        static std::shared_ptr<UnifiedObservableConfigManager> createWithConfig(
            const ConfigManagerConfig &config);

        /**
         * @brief Create a configuration manager for testing
         * @param config_file_path Path to the configuration file (optional)
         * @return Shared pointer to test-configured UnifiedObservableConfigManager
         */
        static std::shared_ptr<UnifiedObservableConfigManager> createForTesting(
            const std::string &config_file_path = "");

        /**
         * @brief Create a configuration manager for production use
         * @param config_file_path Path to the configuration file
         * @param enable_monitoring Whether to enable file monitoring
         * @return Shared pointer to production-configured UnifiedObservableConfigManager
         */
        static std::shared_ptr<UnifiedObservableConfigManager> createForProduction(
            const std::string &config_file_path,
            bool enable_monitoring = true);




        /**
         * @brief Validate configuration settings
         * @param config Configuration to validate
         * @return true if configuration is valid, false otherwise
         */
        static bool validateConfig(const ConfigManagerConfig &config);


    private:
        /**
         * @brief Create the actual UnifiedObservableConfigManager instance
         * @param config Configuration settings
         * @return Raw pointer to UnifiedObservableConfigManager (caller takes ownership)
         */
        static UnifiedObservableConfigManager *createInstance(const ConfigManagerConfig &config);

        /**
         * @brief Configure the created instance with additional settings
         * @param manager The manager instance to configure
         * @param config Configuration settings
         */
        static void configureInstance(UnifiedObservableConfigManager *manager, const ConfigManagerConfig &config);

        /**
         * @brief Set up default properties for the manager
         * @param manager The manager instance to configure
         * @param config Configuration settings
         */
        static void setupDefaultProperties(UnifiedObservableConfigManager *manager, const ConfigManagerConfig &config);

        /**
         * @brief Set up validation callbacks for the manager
         * @param manager The manager instance to configure
         * @param config Configuration settings
         */
        static void setupValidationCallbacks(UnifiedObservableConfigManager *manager, const ConfigManagerConfig &config);

        /**
         * @brief Set up event callbacks for the manager
         * @param manager The manager instance to configure
         * @param config Configuration settings
         */
        static void setupEventCallbacks(UnifiedObservableConfigManager *manager, const ConfigManagerConfig &config);
    };

} // namespace MediaDedup
