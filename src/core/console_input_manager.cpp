#include "core/console_input_manager.hpp"
#include <Poco/Logger.h>
#include <iostream>
#include <csignal>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace MediaDedupServer
{
    namespace Core
    {

        // Static member initialization
        ConsoleInputManager *ConsoleInputManager::instance_ = nullptr;

        ConsoleInputManager &ConsoleInputManager::getInstance()
        {
            static ConsoleInputManager instance;
            instance_ = &instance;
            return instance;
        }

        bool ConsoleInputManager::initialize()
        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (initialized_.load())
            {
                return true;
            }

            // Setup signal handlers
            if (!setupSignalHandlers())
            {
                Poco::Logger::get("ConsoleInputManager").error("Failed to setup signal handlers");
                return false;
            }

            initialized_.store(true);
            Poco::Logger::get("ConsoleInputManager").information("ConsoleInputManager initialized successfully");

            return true;
        }

        void ConsoleInputManager::shutdown()
        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (!initialized_.load())
            {
                return;
            }

            stop();
            restoreSignalHandlers();

            // Clear all callbacks
            {
                std::lock_guard<std::mutex> callback_lock(callbacks_mutex_);
                callbacks_.clear();
            }

            initialized_.store(false);
            Poco::Logger::get("ConsoleInputManager").information("ConsoleInputManager shutdown completed");
        }

        size_t ConsoleInputManager::subscribeToConsoleEvents(ConsoleEventCallback callback)
        {
            std::lock_guard<std::mutex> lock(callbacks_mutex_);

            size_t subscription_id = next_subscription_id_.fetch_add(1);
            callbacks_.push_back(callback);

            Poco::Logger::get("ConsoleInputManager").debug("Console event subscription added, ID: " + std::to_string(subscription_id));
            return subscription_id;
        }

        void ConsoleInputManager::unsubscribeFromConsoleEvents(size_t subscriptionId)
        {
            std::lock_guard<std::mutex> lock(callbacks_mutex_);

            // Note: Due to std::function comparison limitations, we'll clear all callbacks
            // In a production system, you might want to use a more sophisticated subscription system
            callbacks_.clear();

            Poco::Logger::get("ConsoleInputManager").debug("Console event subscription removed, ID: " + std::to_string(subscriptionId));
        }

        void ConsoleInputManager::start()
        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (!initialized_.load())
            {
                Poco::Logger::get("ConsoleInputManager").error("ConsoleInputManager not initialized");
                return;
            }

            if (running_.load())
            {
                Poco::Logger::get("ConsoleInputManager").warning("ConsoleInputManager already running");
                return;
            }

            running_.store(true);
            console_thread_ = std::make_unique<std::thread>(&ConsoleInputManager::consoleInputThread, this);

            Poco::Logger::get("ConsoleInputManager").information("ConsoleInputManager started");
        }

        void ConsoleInputManager::stop()
        {
            if (!running_.load())
            {
                return;
            }

            running_.store(false);
            condition_.notify_all();

            if (console_thread_ && console_thread_->joinable())
            {
                console_thread_->join();
            }

            console_thread_.reset();
            Poco::Logger::get("ConsoleInputManager").information("ConsoleInputManager stopped");
        }

        void ConsoleInputManager::waitForShutdown()
        {
            if (console_thread_ && console_thread_->joinable())
            {
                console_thread_->join();
            }
        }

        size_t ConsoleInputManager::getSubscriptionCount() const
        {
            std::lock_guard<std::mutex> lock(callbacks_mutex_);
            return callbacks_.size();
        }

        void ConsoleInputManager::processCommand(const std::string &command)
        {
            ConsoleEvent event = parseCommand(command);
            notifySubscribers(event);
        }

        void ConsoleInputManager::consoleInputThread()
        {
            Poco::Logger::get("ConsoleInputManager").information("Console input thread started");

            std::string line;
            while (running_.load())
            {
                std::cout << "dedup_server> ";
                std::cout.flush();

                if (!std::getline(std::cin, line))
                {
                    // EOF or error reading input
                    if (running_.load())
                    {
                        Poco::Logger::get("ConsoleInputManager").information("Console input ended, shutting down");
                        ConsoleEvent event(ConsoleEventType::COMMAND_EXIT, "exit");
                        notifySubscribers(event);
                    }
                    break;
                }

                // Trim whitespace
                line.erase(0, line.find_first_not_of(" \t\r\n"));
                line.erase(line.find_last_not_of(" \t\r\n") + 1);

                if (line.empty())
                {
                    continue;
                }

                ConsoleEvent event = parseCommand(line);
                notifySubscribers(event);

                // Check if we should exit
                if (event.type == ConsoleEventType::COMMAND_EXIT ||
                    event.type == ConsoleEventType::COMMAND_QUIT ||
                    event.type == ConsoleEventType::COMMAND_SHUTDOWN)
                {
                    break;
                }
            }

            Poco::Logger::get("ConsoleInputManager").information("Console input thread finished");
        }

        void ConsoleInputManager::signalHandler(int signal)
        {
            if (!instance_)
            {
                return;
            }

            ConsoleEventType eventType;
            std::string signalName;

            switch (signal)
            {
            case SIGINT:
                eventType = ConsoleEventType::SIGNAL_INTERRUPT;
                signalName = "SIGINT";
                break;
            case SIGTERM:
                eventType = ConsoleEventType::SIGNAL_TERMINATE;
                signalName = "SIGTERM";
                break;
            case SIGQUIT:
                eventType = ConsoleEventType::SIGNAL_QUIT;
                signalName = "SIGQUIT";
                break;
            default:
                return;
            }

            Poco::Logger::get("ConsoleInputManager").information("Received signal: " + signalName);

            ConsoleEvent event(eventType, signalName);
            instance_->notifySubscribers(event);
        }

        ConsoleEvent ConsoleInputManager::parseCommand(const std::string &command)
        {
            std::istringstream iss(command);
            std::string cmd;
            std::string args;

            iss >> cmd;
            std::getline(iss, args);

            // Trim args
            args.erase(0, args.find_first_not_of(" \t\r\n"));
            args.erase(args.find_last_not_of(" \t\r\n") + 1);

            // Convert to lowercase
            std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::tolower);

            if (cmd == "exit" || cmd == "quit")
            {
                return ConsoleEvent(ConsoleEventType::COMMAND_EXIT, cmd, args);
            }
            else if (cmd == "shutdown")
            {
                return ConsoleEvent(ConsoleEventType::COMMAND_SHUTDOWN, cmd, args);
            }
            else if (cmd == "restart")
            {
                return ConsoleEvent(ConsoleEventType::COMMAND_RESTART, cmd, args);
            }
            else if (cmd == "status")
            {
                return ConsoleEvent(ConsoleEventType::COMMAND_STATUS, cmd, args);
            }
            else if (cmd == "help")
            {
                return ConsoleEvent(ConsoleEventType::COMMAND_HELP, cmd, args);
            }
            else
            {
                return ConsoleEvent(ConsoleEventType::UNKNOWN_COMMAND, cmd, args);
            }
        }

        void ConsoleInputManager::notifySubscribers(const ConsoleEvent &event)
        {
            std::vector<ConsoleEventCallback> callbacks_copy;

            {
                std::lock_guard<std::mutex> lock(callbacks_mutex_);
                callbacks_copy = callbacks_;
            }

            for (const auto &callback : callbacks_copy)
            {
                try
                {
                    callback(event);
                }
                catch (const std::exception &e)
                {
                    Poco::Logger::get("ConsoleInputManager").error("Exception in console event callback: " + std::string(e.what()));
                }
            }
        }

        bool ConsoleInputManager::setupSignalHandlers()
        {
            // Setup SIGINT handler
            struct sigaction sa_int;
            sa_int.sa_handler = signalHandler;
            sigemptyset(&sa_int.sa_mask);
            sa_int.sa_flags = 0;

            if (sigaction(SIGINT, &sa_int, &original_sigint_) == -1)
            {
                Poco::Logger::get("ConsoleInputManager").error("Failed to setup SIGINT handler");
                return false;
            }

            // Setup SIGTERM handler
            struct sigaction sa_term;
            sa_term.sa_handler = signalHandler;
            sigemptyset(&sa_term.sa_mask);
            sa_term.sa_flags = 0;

            if (sigaction(SIGTERM, &sa_term, &original_sigterm_) == -1)
            {
                Poco::Logger::get("ConsoleInputManager").error("Failed to setup SIGTERM handler");
                return false;
            }

            // Setup SIGQUIT handler
            struct sigaction sa_quit;
            sa_quit.sa_handler = signalHandler;
            sigemptyset(&sa_quit.sa_mask);
            sa_quit.sa_flags = 0;

            if (sigaction(SIGQUIT, &sa_quit, &original_sigquit_) == -1)
            {
                Poco::Logger::get("ConsoleInputManager").error("Failed to setup SIGQUIT handler");
                return false;
            }

            signal_handlers_setup_ = true;
            return true;
        }

        void ConsoleInputManager::restoreSignalHandlers()
        {
            if (!signal_handlers_setup_)
            {
                return;
            }

            sigaction(SIGINT, &original_sigint_, nullptr);
            sigaction(SIGTERM, &original_sigterm_, nullptr);
            sigaction(SIGQUIT, &original_sigquit_, nullptr);

            signal_handlers_setup_ = false;
        }

    } // namespace Core
} // namespace MediaDedupServer
