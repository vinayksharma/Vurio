#pragma once

#include <memory>
#include <string>
#include <filesystem>
#include <fstream>
#include <Poco/Logger.h>

#include "config/unified_observable_config.hpp"

namespace MediaDedup
{

    /**
     * @brief High-level database management service
     *
     * Encapsulates database-related management tasks that are not specific to
     * a single connection/session, such as ensuring the database file exists
     * at the configured location and preparing directories.
     */
    class DatabaseService
    {
    public:
        explicit DatabaseService(std::shared_ptr<UnifiedObservableConfigManager> config_manager)
            : config_manager_(std::move(config_manager)), logger_(Poco::Logger::get("DatabaseService")) {}

        /**
         * @brief Ensure the database file exists using the configured path
         *
         * Reads "database.path" from configuration (or uses a sensible default),
         * creates parent directories if necessary, and creates an empty file if
         * it does not already exist.
         *
         * @return true on success, false otherwise
         */
        bool ensureDatabaseFileExists();

        /**
         * @brief Get the resolved database path used by the service
         */
        std::string getDatabasePath() const { return database_path_; }

    private:
        std::shared_ptr<UnifiedObservableConfigManager> config_manager_;
        std::string database_path_;
        Poco::Logger &logger_;
    };

} // namespace MediaDedup

