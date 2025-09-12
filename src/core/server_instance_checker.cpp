#include "core/instance_checker.hpp"
#include "config/unified_observable_config.hpp"
#include <Poco/Logger.h>
#include <cstdio>
#include <vector>
#include <sstream>
#include <stdexcept>

namespace MediaDedup
{
        InstanceChecker::InstanceChecker(std::shared_ptr<UnifiedObservableConfigManager> config_manager)
            : config_manager_(std::move(config_manager))
        {
        }

        bool InstanceChecker::checkForExistingInstances()
        {
            bool instance_check_enabled = config_manager_->getPropertyValue<bool>("server.instanceCheck.enabled", true);
            if (!instance_check_enabled)
            {
                Poco::Logger::get("InstanceChecker").debug("Instance checking is disabled, proceeding with startup");
                return true;
            }

            pid_t current_pid = getCurrentPid();
            std::string process_name = config_manager_->getPropertyValue<std::string>("server.processName", "media_dedup_server");
            
            std::string result = executePgrep(process_name);
            if (result.empty())
            {
                Poco::Logger::get("InstanceChecker").debug("No existing instances found, proceeding with startup");
                return true;
            }

            std::vector<pid_t> existing_pids = parsePids(result, current_pid);
            if (!existing_pids.empty())
            {
                Poco::Logger &logger = Poco::Logger::get("InstanceChecker");
                logger.error("Found %d existing instance(s) of Media Deduplication Server:", static_cast<int>(existing_pids.size()));
                for (pid_t pid : existing_pids)
                {
                    logger.error("  - PID: %d", static_cast<int>(pid));
                }
                logger.error("Please stop existing instances before starting a new one.");
                logger.error("Use './scripts/stopall' to stop all instances.");
                return false;
            }

            Poco::Logger::get("InstanceChecker").debug("No existing instances found, proceeding with startup");
            return true;
        }

        pid_t InstanceChecker::getCurrentPid() const
        {
            return getpid();
        }

        std::string InstanceChecker::executePgrep(const std::string &process_name) const
        {
            std::string command = "pgrep -f " + process_name + " 2>/dev/null || true";
            FILE *pipe = popen(command.c_str(), "r");
            if (!pipe)
            {
                Poco::Logger::get("InstanceChecker").warning("Could not check for existing instances (popen failed)");
                return "";
            }

            std::string result;
            int buffer_size = config_manager_->getPropertyValue<int>("server.instanceCheck.bufferSize", 128);
            std::vector<char> buffer(buffer_size);
            while (fgets(buffer.data(), buffer_size, pipe) != nullptr)
            {
                result += std::string(buffer.data());
            }
            pclose(pipe);

            return result;
        }

        std::vector<pid_t> InstanceChecker::parsePids(const std::string &output, pid_t current_pid) const
        {
            std::vector<pid_t> existing_pids;
            std::istringstream iss(output);
            std::string line;

            while (std::getline(iss, line))
            {
                if (!line.empty())
                {
                    try
                    {
                        pid_t pid = std::stoi(line);
                        if (pid != current_pid)
                        {
                            existing_pids.push_back(pid);
                        }
                    }
                    catch (const std::exception &e)
                    {
                        // Ignore invalid PIDs
                        continue;
                    }
                }
            }

            return existing_pids;
    }
}
