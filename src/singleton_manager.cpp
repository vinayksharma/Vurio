#include "core/singleton_manager.hpp"
#include "logging/logger.hpp"
#include <iostream>
#include <fstream>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <memory>

// FIXED: Using smart pointer instead of raw pointer
static std::unique_ptr<SingletonManager> instance;

// Static member definitions
std::string SingletonManager::pid_file_path = "";
std::ofstream SingletonManager::pid_file;
bool SingletonManager::is_running = false;

SingletonManager &SingletonManager::getInstance()
{
    if (!instance)
    {
        instance = std::make_unique<SingletonManager>(); // FIXED: Using smart pointer
    }
    return *instance;
}

void SingletonManager::initialize(const std::string &path)
{
    pid_file_path = path;
}

bool SingletonManager::isAnotherInstanceRunning()
{
    if (pid_file_path.empty())
    {
        return false;
    }

    // Check if PID file exists (regardless of whether process is running)
    std::ifstream file(pid_file_path);
    if (!file.is_open())
    {
        return false;
    }
    file.close();

    return true;
}

bool SingletonManager::createPidFile()
{
    if (pid_file_path.empty())
    {
        return false;
    }

    // Try to create PID file with exclusive access
    int fd = open(pid_file_path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd == -1)
    {
        // File already exists or cannot be created
        return false;
    }

    // Write current PID to file
    pid_t current_pid = getpid();
    std::string pid_str = std::to_string(current_pid) + "\n";
    write(fd, pid_str.c_str(), pid_str.length());
    close(fd);

    // Open the file for the ofstream to use
    pid_file.open(pid_file_path, std::ios::out | std::ios::app);
    if (!pid_file.is_open())
    {
        unlink(pid_file_path.c_str());
        return false;
    }

    is_running = true;

    // Set up signal handlers for graceful shutdown
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    signal(SIGQUIT, signalHandler);

    return true;
}

void SingletonManager::removePidFile()
{
    if (is_running && pid_file.is_open())
    {
        pid_file.close();
        unlink(pid_file_path.c_str());
        is_running = false;
    }
}

bool SingletonManager::shutdownExistingInstance()
{
    if (!isAnotherInstanceRunning())
    {
        return true; // No instance running
    }

    pid_t existing_pid = getPidFromFile();
    if (existing_pid <= 0)
    {
        // Invalid PID in file, remove the stale PID file
        Logger::info("Invalid PID in file, removing stale PID file...");
        unlink(pid_file_path.c_str());
        return true;
    }

    // Check if process is actually running
    if (kill(existing_pid, 0) != 0)
    {
        // Process is not running, remove stale PID file
        Logger::info("Process " + std::to_string(existing_pid) + " is not running, removing stale PID file...");
        unlink(pid_file_path.c_str());
        return true;
    }

    // Send SIGTERM to existing process
    if (kill(existing_pid, SIGTERM) == 0)
    {
        Logger::info("Sent shutdown signal to existing instance (PID: " + std::to_string(existing_pid) + ")");

        // Wait a bit for graceful shutdown
        sleep(2);

        // Check if process is still running
        if (kill(existing_pid, 0) == 0)
        {
            Logger::info("Existing instance still running, sending SIGKILL...");
            kill(existing_pid, SIGKILL);
            sleep(1);
        }

        return true;
    }

    return false;
}

bool SingletonManager::isPidFileValid()
{
    if (pid_file_path.empty())
    {
        return false;
    }

    std::ifstream file(pid_file_path);
    if (!file.is_open())
    {
        return false;
    }

    pid_t pid;
    file >> pid;
    file.close();

    if (pid <= 0)
    {
        return false;
    }

    // Check if process is still running
    return (kill(pid, 0) == 0);
}

pid_t SingletonManager::getPidFromFile()
{
    if (pid_file_path.empty())
    {
        return -1;
    }

    std::ifstream file(pid_file_path);
    if (!file.is_open())
    {
        return -1;
    }

    pid_t pid;
    file >> pid;
    file.close();

    return pid;
}

void SingletonManager::signalHandler(int signal)
{
    Logger::info("Received signal " + std::to_string(signal) + ", shutting down gracefully...");

    // Safely remove PID file
    try
    {
        SingletonManager::getInstance().removePidFile();
    }
    catch (...)
    {
        // Ignore any exceptions during cleanup
        Logger::warn("Error during PID file cleanup");
    }

    // Flush any pending output
    std::cout.flush();
    std::cerr.flush();

    // Exit cleanly without calling destructors that might cause issues
    _exit(0);
}

void SingletonManager::cleanup()
{
    if (instance)
    {
        instance->removePidFile();
        instance.reset(); // Reset smart pointer
    }
}