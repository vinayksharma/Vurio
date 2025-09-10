#pragma once

#include <string>
#include <memory>

namespace MediaDedup
{
    class UnifiedObservableConfigManager;

    /**
     * @brief Handles server configuration management
     */
    class ServerConfigManager
        {
        public:
            /**
             * @brief Constructor
             * @param config_manager Configuration manager instance
             */
            explicit ServerConfigManager(std::shared_ptr<UnifiedObservableConfigManager> config_manager);

            /**
             * @brief Destructor
             */
            ~ServerConfigManager() = default;

            /**
             * @brief Apply default values for configuration-related members
             */
            void applyDefaultConfigValues();

            /**
             * @brief Get server host
             * @return Server host
             */
            const std::string &getServerHost() const { return server_host_; }

            /**
             * @brief Get server port
             * @return Server port
             */
            uint16_t getServerPort() const { return server_port_; }

            /**
             * @brief Get database path
             * @return Database path
             */
            const std::string &getDatabasePath() const { return database_path_; }

            /**
             * @brief Get logging level
             * @return Logging level
             */
            const std::string &getLoggingLevel() const { return logging_level_; }

            /**
             * @brief Get server mode
             * @return Server mode
             */
            const std::string &getServerMode() const { return server_mode_; }

            /**
             * @brief Set server host
             * @param host Server host
             */
            void setServerHost(const std::string &host) { server_host_ = host; }

            /**
             * @brief Set server port
             * @param port Server port
             */
            void setServerPort(uint16_t port) { server_port_ = port; }

            /**
             * @brief Set database path
             * @param path Database path
             */
            void setDatabasePath(const std::string &path) { database_path_ = path; }

            /**
             * @brief Set logging level
             * @param level Logging level
             */
            void setLoggingLevel(const std::string &level) { logging_level_ = level; }

            /**
             * @brief Set server mode
             * @param mode Server mode
             */
            void setServerMode(const std::string &mode) { server_mode_ = mode; }

        private:
            std::shared_ptr<UnifiedObservableConfigManager> config_manager_;
            
            // Configuration values
            std::string server_host_;
            uint16_t server_port_ = 0;
            std::string database_path_;
            std::string logging_level_;
            std::string server_mode_;
    };
}
