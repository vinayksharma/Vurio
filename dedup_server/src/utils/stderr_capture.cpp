#include "utils/stderr_capture.hpp"
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <array>

namespace MediaDedup
{
    // Thread-local flag to prevent nested captures
    thread_local bool StderrCapture::thread_is_capturing_ = false;

    // Global mutex to serialize stderr captures
    std::mutex StderrCapture::global_capture_mutex_;

    StderrCapture::StderrCapture()
        : saved_stderr_(-1), pipe_fds_{-1, -1}, is_active_(false)
    {
        // Check if this thread is already capturing
        if (thread_is_capturing_)
        {
            // Nested capture - do nothing, will be a no-op
            return;
        }

        // Lock global mutex - stderr is process-wide, must serialize captures
        global_capture_mutex_.lock();

        // Create pipe for capturing stderr
        if (pipe(pipe_fds_) != 0)
        {
            // Pipe creation failed - unlock and abort
            global_capture_mutex_.unlock();
            return;
        }

        // Save current stderr
        saved_stderr_ = dup(STDERR_FILENO);
        if (saved_stderr_ == -1)
        {
            // Failed to save stderr - close pipe, unlock, and abort
            close(pipe_fds_[0]);
            close(pipe_fds_[1]);
            pipe_fds_[0] = pipe_fds_[1] = -1;
            global_capture_mutex_.unlock();
            return;
        }

        // Redirect stderr to pipe write end
        if (dup2(pipe_fds_[1], STDERR_FILENO) == -1)
        {
            // Failed to redirect - restore, unlock, and abort
            close(saved_stderr_);
            close(pipe_fds_[0]);
            close(pipe_fds_[1]);
            saved_stderr_ = -1;
            pipe_fds_[0] = pipe_fds_[1] = -1;
            global_capture_mutex_.unlock();
            return;
        }

        // Make read end non-blocking for easier reading
        int flags = fcntl(pipe_fds_[0], F_GETFL, 0);
        fcntl(pipe_fds_[0], F_SETFL, flags | O_NONBLOCK);

        // Success - mark as active
        is_active_ = true;
        thread_is_capturing_ = true;
    }

    StderrCapture::~StderrCapture()
    {
        if (!is_active_)
        {
            // If never activated, might still hold mutex - check and unlock
            // This can happen if nested capture was detected
            return;
        }

        // Flush stderr to ensure all data is in the pipe
        fflush(stderr);

        // Restore original stderr
        if (saved_stderr_ != -1)
        {
            dup2(saved_stderr_, STDERR_FILENO);
            close(saved_stderr_);
        }

        // Read captured data from pipe
        captured_ = readFromPipe();

        // Close pipe
        if (pipe_fds_[0] != -1)
        {
            close(pipe_fds_[0]);
        }
        if (pipe_fds_[1] != -1)
        {
            close(pipe_fds_[1]);
        }

        // Reset thread flag
        thread_is_capturing_ = false;
        is_active_ = false;

        // Unlock global mutex
        global_capture_mutex_.unlock();
    }

    std::string StderrCapture::getOutput() const
    {
        if (!is_active_)
        {
            return captured_;
        }

        // Flush stderr to ensure all data is in the pipe
        fflush(stderr);

        // Read available data from pipe without closing it
        if (pipe_fds_[0] != -1)
        {
            std::string result;
            std::array<char, 4096> buffer;

            // Read all available data (non-blocking)
            while (true)
            {
                ssize_t bytes_read = read(pipe_fds_[0], buffer.data(), buffer.size());

                if (bytes_read > 0)
                {
                    result.append(buffer.data(), bytes_read);
                }
                else
                {
                    // No more data available or error
                    break;
                }
            }

            return result;
        }

        return "";
    }

    std::string StderrCapture::readFromPipe()
    {
        if (pipe_fds_[0] == -1)
        {
            return "";
        }

        std::string result;
        std::array<char, 4096> buffer;

        // Close write end to allow EOF on read end
        if (pipe_fds_[1] != -1)
        {
            close(pipe_fds_[1]);
            pipe_fds_[1] = -1;
        }

        // Read all available data
        while (true)
        {
            ssize_t bytes_read = read(pipe_fds_[0], buffer.data(), buffer.size());

            if (bytes_read > 0)
            {
                result.append(buffer.data(), bytes_read);
            }
            else if (bytes_read == 0)
            {
                // EOF reached
                break;
            }
            else
            {
                // Error or would block (EAGAIN/EWOULDBLOCK for non-blocking)
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    // No more data available
                    break;
                }
                // Other error - stop reading
                break;
            }
        }

        return result;
    }

} // namespace MediaDedup
