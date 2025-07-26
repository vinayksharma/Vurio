#pragma once

#include <string>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#include <iostream>

class SingletonManager
{
private:
    static SingletonManager *instance;
    static std::string pid_file_path;
    static std::ofstream pid_file;
    static bool is_running;

    SingletonManager() = default;
    ~SingletonManager() = default;
    SingletonManager(const SingletonManager &) = delete;
    SingletonManager &operator=(const SingletonManager &) = delete;

public:
    static SingletonManager &getInstance();

    // Check if another instance is running
    bool isAnotherInstanceRunning();

    // Create PID file and lock it
    bool createPidFile();

    // Remove PID file
    void removePidFile();

    // Send shutdown signal to existing instance
    bool shutdownExistingInstance();

    // Check if PID file exists and process is running
    bool isPidFileValid();

    // Get PID from file
    pid_t getPidFromFile();

    // Graceful shutdown handler
    static void signalHandler(int signal);

    // Initialize singleton with PID file path
    static void initialize(const std::string &pid_file_path);

    // Cleanup singleton
    static void cleanup();
};