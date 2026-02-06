#pragma once

#include <string>
#include <memory>
#include <vector>
#include <unistd.h>

namespace MediaDedup
{
    class UnifiedObservableConfigManager;

    /**
     * @brief Handles checking for existing server instances
     */
    class InstanceChecker
        {
        public:
            /**
             * @brief Constructor
             * @param config_manager Configuration manager instance
             */
            explicit InstanceChecker(std::shared_ptr<UnifiedObservableConfigManager> config_manager);

            /**
             * @brief Destructor
             */
            ~InstanceChecker() = default;

            /**
             * @brief Check for existing instances of the server
             * @return true if no other instances are running, false if another instance exists
             */
            bool checkForExistingInstances();

        private:
            std::shared_ptr<UnifiedObservableConfigManager> config_manager_;
            
            /**
             * @brief Get the current process ID
             * @return Current process ID
             */
            pid_t getCurrentPid() const;

            /**
             * @brief Execute pgrep command to find running instances
             * @param process_name Process name to search for
             * @return Output from pgrep command
             */
            std::string executePgrep(const std::string &process_name) const;

            /**
             * @brief Parse PIDs from pgrep output
             * @param output Output from pgrep command
             * @param current_pid Current process ID to exclude
             * @return Vector of PIDs found
             */
            std::vector<pid_t> parsePids(const std::string &output, pid_t current_pid) const;
    };
}
