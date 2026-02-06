#include "core/config_manager.hpp"
#include "config/unified_observable_config.hpp"

namespace MediaDedup
{
        ServerConfigManager::ServerConfigManager(std::shared_ptr<UnifiedObservableConfigManager> config_manager)
            : config_manager_(std::move(config_manager))
        {
        }

        void ServerConfigManager::applyDefaultConfigValues()
        {
            // Keep all default config-related values in one place
            if (server_host_.empty())
            {
                server_host_ = "0.0.0.0";
            }
            if (server_port_ == 0)
            {
                server_port_ = 8080;
            }
            if (database_path_.empty())
            {
                database_path_ = "data/dedup_server.db";
            }
            if (logging_level_.empty())
            {
                logging_level_ = "info";
            }
            if (server_mode_.empty())
            {
                server_mode_ = "FAST"; // FAST | BALANCED | QUALITY
            }
    }
}
