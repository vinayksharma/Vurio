#pragma once

#include <string>
#include <mutex>

namespace MediaDedup
{
    /**
     * @brief RAII-based stderr capture utility for thread-safe stderr redirection
     *
     * Captures stderr output during library calls to preserve detailed error messages
     * that would otherwise be lost. Each thread gets its own capture context.
     *
     * Usage:
     *   {
     *       StderrCapture capture;
     *       bool success = SomeLibraryCall();
     *       if (!success) {
     *           std::string error_details = capture.getOutput();
     *           // Use error_details in error logging
     *       }
     *   } // Stderr automatically restored here
     */
    class StderrCapture
    {
    public:
        /**
         * @brief Start capturing stderr for this thread
         *
         * Redirects stderr to an internal pipe. Original stderr is saved
         * and will be restored on destruction.
         */
        StderrCapture();

        /**
         * @brief Stop capturing and restore original stderr
         *
         * Reads all captured data from the pipe and restores the original
         * stderr file descriptor. Safe to call even if construction failed.
         */
        ~StderrCapture();

        // Non-copyable, non-movable (manages file descriptors)
        StderrCapture(const StderrCapture &) = delete;
        StderrCapture &operator=(const StderrCapture &) = delete;
        StderrCapture(StderrCapture &&) = delete;
        StderrCapture &operator=(StderrCapture &&) = delete;

        /**
         * @brief Get the captured stderr output
         * @return All text written to stderr since construction
         */
        std::string getOutput() const;

    private:
        int saved_stderr_;     ///< Original stderr file descriptor
        int pipe_fds_[2];      ///< Pipe for capturing: [0]=read, [1]=write
        std::string captured_; ///< Buffered captured output
        bool is_active_;       ///< Whether capture is currently active

        /**
         * @brief Read all available data from the capture pipe
         * @return String containing all captured data
         */
        std::string readFromPipe();

        /**
         * @brief Thread-local flag to prevent nested captures
         *
         * Each thread can only have one active StderrCapture at a time.
         * Nested captures are silently ignored.
         */
        static thread_local bool thread_is_capturing_;

        /**
         * @brief Global mutex to serialize stderr captures across threads
         *
         * Since stderr (fd 2) is process-wide, we must serialize captures
         * to prevent threads from interfering with each other.
         */
        static std::mutex global_capture_mutex_;
    };

} // namespace MediaDedup
