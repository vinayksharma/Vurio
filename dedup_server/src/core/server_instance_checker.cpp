#include "core/instance_checker.hpp"
#include "config/unified_observable_config.hpp"
#include <Poco/Logger.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <sstream>
#include <stdexcept>

#include <climits>
#ifdef __APPLE__
#include <libproc.h>
#endif

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

            // Only a process running this exact binary is a duplicate of this
            // instance; servers from other worktrees may run concurrently.
            std::string own_path = getExecutablePath(current_pid);
            std::vector<pid_t> conflicting_pids;
            std::vector<pid_t> sibling_pids;
            for (pid_t pid : parsePids(result, current_pid))
            {
                std::string other_path = getExecutablePath(pid);
                if (!own_path.empty() && other_path == own_path)
                {
                    conflicting_pids.push_back(pid);
                }
                else
                {
                    sibling_pids.push_back(pid);
                }
            }

            Poco::Logger &logger = Poco::Logger::get("InstanceChecker");
            for (pid_t pid : sibling_pids)
            {
                logger.information("Ignoring server instance from another worktree (PID: %d)", static_cast<int>(pid));
            }

            if (!conflicting_pids.empty())
            {
                logger.error("Found %d existing instance(s) of this server binary:", static_cast<int>(conflicting_pids.size()));
                for (pid_t pid : conflicting_pids)
                {
                    logger.error("  - PID: %d", static_cast<int>(pid));
                }
                logger.error("Please stop the existing instance before starting a new one.");
                logger.error("Use './run.sh stop' in this worktree, or './killall' at the repo root to stop all instances.");
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

        std::string InstanceChecker::getExecutablePath(pid_t pid) const
        {
#ifdef __APPLE__
            char path_buffer[PROC_PIDPATHINFO_MAXSIZE] = {0};
            if (proc_pidpath(pid, path_buffer, sizeof(path_buffer)) <= 0)
            {
                return "";
            }
#else
            char link_path[64];
            snprintf(link_path, sizeof(link_path), "/proc/%d/exe", static_cast<int>(pid));
            char path_buffer[PATH_MAX] = {0};
            ssize_t len = readlink(link_path, path_buffer, sizeof(path_buffer) - 1);
            if (len <= 0)
            {
                return "";
            }
            path_buffer[len] = '\0';
#endif
            char resolved[PATH_MAX] = {0};
            if (realpath(path_buffer, resolved) != nullptr)
            {
                return std::string(resolved);
            }
            return std::string(path_buffer);
        }
}
