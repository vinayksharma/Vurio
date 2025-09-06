#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <atomic>
#include <condition_variable>
#include <queue>
#include <string>

namespace MediaDedupServer
{
    namespace Core
    {

        /**
         * @brief Console input event types
         */
        enum class ConsoleEventType
        {
            SIGNAL_INTERRUPT, // Ctrl+C (SIGINT)
            SIGNAL_TERMINATE, // SIGTERM
            SIGNAL_QUIT,      // SIGQUIT
            COMMAND_EXIT,     // 'exit' command
            COMMAND_QUIT,     // 'quit' command
            COMMAND_SHUTDOWN, // 'shutdown' command
            COMMAND_RESTART,  // 'restart' command
            COMMAND_STATUS,   // 'status' command
            COMMAND_HELP,     // 'help' command
            UNKNOWN_COMMAND   // Unknown command
        };

        /**
         * @brief Console input event structure
         */
        struct ConsoleEvent
        {
            ConsoleEventType type;
            std::string command;
            std::string arguments;
            std::chrono::system_clock::time_point timestamp;

            ConsoleEvent(ConsoleEventType t, const std::string &cmd = "", const std::string &args = "")
                : type(t), command(cmd), arguments(args), timestamp(std::chrono::system_clock::now()) {}
        };

        /**
         * @brief Console input event callback type
         */
        using ConsoleEventCallback = std::function<void(const ConsoleEvent &)>;

        /**
         * @brief Singleton class for managing console input and signals
         *
         * This class provides a thread-safe observable interface for console events.
         * Components can subscribe to receive notifications about console input events
         * such as Ctrl+C, commands, and signals.
         */
        class ConsoleInputManager
        {
        public:
            /**
             * @brief Get the singleton instance
             * @return Reference to the singleton instance
             */
            static ConsoleInputManager &getInstance();

            /**
             * @brief Initialize the console input manager
             * @return true if initialization successful, false otherwise
             */
            bool initialize();

            /**
             * @brief Shutdown the console input manager
             */
            void shutdown();

            /**
             * @brief Check if the manager is running
             * @return true if running, false otherwise
             */
            bool isRunning() const { return running_.load(); }

            /**
             * @brief Subscribe to console events
             * @param callback Function to call when console events occur
             * @return Subscription ID for later unsubscription
             */
            size_t subscribeToConsoleEvents(ConsoleEventCallback callback);

            /**
             * @brief Unsubscribe from console events
             * @param subscriptionId ID returned from subscribeToConsoleEvents
             */
            void unsubscribeFromConsoleEvents(size_t subscriptionId);

            /**
             * @brief Start the console input processing thread
             */
            void start();

            /**
             * @brief Stop the console input processing thread
             */
            void stop();

            /**
             * @brief Wait for the console input thread to finish
             */
            void waitForShutdown();

            /**
             * @brief Get the current subscription count
             * @return Number of active subscriptions
             */
            size_t getSubscriptionCount() const;

            /**
             * @brief Process a command string (for testing or programmatic use)
             * @param command Command string to process
             */
            void processCommand(const std::string &command);

        private:
            ConsoleInputManager() = default;
            ~ConsoleInputManager() = default;

            // Disable copy constructor and assignment operator
            ConsoleInputManager(const ConsoleInputManager &) = delete;
            ConsoleInputManager &operator=(const ConsoleInputManager &) = delete;

            /**
             * @brief Console input processing thread function
             */
            void consoleInputThread();

            /**
             * @brief Signal handler function
             * @param signal Signal number
             */
            static void signalHandler(int signal);

            /**
             * @brief Parse command string and create appropriate event
             * @param command Command string to parse
             * @return ConsoleEvent object
             */
            ConsoleEvent parseCommand(const std::string &command);

            /**
             * @brief Notify all subscribers of a console event
             * @param event Event to broadcast
             */
            void notifySubscribers(const ConsoleEvent &event);

            /**
             * @brief Setup signal handlers
             * @return true if successful, false otherwise
             */
            bool setupSignalHandlers();

            /**
             * @brief Restore original signal handlers
             */
            void restoreSignalHandlers();

            // Static instance pointer for signal handler access
            static ConsoleInputManager *instance_;

            // Thread safety
            mutable std::mutex mutex_;
            std::condition_variable condition_;

            // State management
            std::atomic<bool> running_{false};
            std::atomic<bool> initialized_{false};
            std::atomic<bool> exit_requested_by_signal_{false};

            // Threading
            std::unique_ptr<std::thread> console_thread_;

            // Event system
            std::vector<ConsoleEventCallback> callbacks_;
            std::queue<ConsoleEvent> event_queue_;
            std::mutex event_queue_mutex_;

            // Subscription management
            std::atomic<size_t> next_subscription_id_{1};
            mutable std::mutex callbacks_mutex_;

            // Signal handling
            struct sigaction original_sigint_;
            struct sigaction original_sigterm_;
            struct sigaction original_sigquit_;
            bool signal_handlers_setup_{false};
        };

    } // namespace Core
} // namespace MediaDedupServer
