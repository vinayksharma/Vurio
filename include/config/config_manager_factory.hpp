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
        bool auto_save = true;
        std::string log_level = "info";

        // File monitoring settings
        bool enable_file_watching = true;
        std::chrono::milliseconds file_check_interval = std::chrono::milliseconds(500);

        // Validation settings
        bool strict_validation = false;
        bool validate_on_load = true;
        bool validate_on_save = true;

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
         * @brief Create a default configuration manager
         * @param config_file_path Path to the configuration file
         * @return Shared pointer to configured UnifiedObservableConfigManager
         */
        static std::shared_ptr<UnifiedObservableConfigManager> createDefault(
            const std::string &config_file_path);

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
         * @brief Create a configuration manager for development use
         * @param config_file_path Path to the configuration file
         * @return Shared pointer to development-configured UnifiedObservableConfigManager
         */
        static std::shared_ptr<UnifiedObservableConfigManager> createForDevelopment(
            const std::string &config_file_path);

        /**
         * @brief Create a configuration manager with custom component factories
         * @param config Configuration settings
         * @param file_manager_factory Custom file manager factory
         * @param validator_factory Custom validator factory
         * @param event_manager_factory Custom event manager factory
         * @return Shared pointer to configured UnifiedObservableConfigManager
         */
        static std::shared_ptr<UnifiedObservableConfigManager> createWithCustomComponents(
            const ConfigManagerConfig &config,
            std::function<std::unique_ptr<ConfigFileManager>(const std::string &)> file_manager_factory = nullptr,
            std::function<std::unique_ptr<ConfigValidator>()> validator_factory = nullptr,
            std::function<std::unique_ptr<ConfigEventManager>()> event_manager_factory = nullptr);

        /**
         * @brief Get default configuration for a specific environment
         * @param environment Environment name ("development", "testing", "production")
         * @return Default configuration for the environment
         */
        static ConfigManagerConfig getDefaultConfig(const std::string &environment);

        /**
         * @brief Validate configuration settings
         * @param config Configuration to validate
         * @return true if configuration is valid, false otherwise
         */
        static bool validateConfig(const ConfigManagerConfig &config);

        /**
         * @brief Create a configuration manager from environment variables
         * @param config_file_path Path to the configuration file
         * @return Shared pointer to environment-configured UnifiedObservableConfigManager
         */
        static std::shared_ptr<UnifiedObservableConfigManager> createFromEnvironment(
            const std::string &config_file_path);

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
