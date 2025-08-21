#pragma once
#include <signal.h>
#include <execinfo.h>
#include <cxxabi.h>
#include <dlfcn.h>
#include <sstream>
#include <atomic>
#include <functional>
#include <vector>
#include <memory>
#include <map>
#include <algorithm>
#include "logging/logger.hpp"

class CrashRecovery
{
public:
    // Signal handler function type
    using SignalHandlerFunc = std::function<void(int, const std::string &)>;

    // Initialize crash recovery system
    static void initialize()
    {
        installSignalHandlers();
        setupAlternateSignalStack();
        Logger::info("Crash recovery system initialized");
    }

    // Install signal handlers for common crash signals
    static void installSignalHandlers()
    {
        // Install handlers for common crash signals
        signal(SIGSEGV, handleSegfault);
        signal(SIGBUS, handleBusError);
        signal(SIGFPE, handleFloatingPoint);
        signal(SIGILL, handleIllegalInstruction);
        signal(SIGABRT, handleAbort);
        signal(SIGTERM, handleTermination);
        signal(SIGINT, handleInterrupt);

        Logger::debug("Signal handlers installed for crash recovery");
    }

    // Set custom signal handler for specific signal
    static void setCustomSignalHandler(int signal_num, SignalHandlerFunc handler)
    {
        custom_handlers_[signal_num] = handler;
        signal(signal_num, [](int sig)
               {
            auto it = custom_handlers_.find(sig);
            if (it != custom_handlers_.end()) {
                it->second(sig, getSignalName(sig));
            } });
    }

    // Enable/disable stack trace printing
    static void setStackTraceEnabled(bool enabled)
    {
        stack_trace_enabled_.store(enabled);
    }

    // Set maximum stack trace depth
    static void setMaxStackTraceDepth(int depth)
    {
        max_stack_trace_depth_.store(depth);
    }

    // Get current stack trace as string
    static std::string getCurrentStackTrace()
    {
        return generateStackTrace(max_stack_trace_depth_.load());
    }

    // Set recovery callback for specific signal
    static void setRecoveryCallback(int signal_num, std::function<bool()> callback)
    {
        recovery_callbacks_[signal_num] = callback;
    }

    // Attempt recovery for a specific signal
    static bool attemptRecovery(int signal_num)
    {
        auto it = recovery_callbacks_.find(signal_num);
        if (it != recovery_callbacks_.end())
        {
            try
            {
                return it->second();
            }
            catch (...)
            {
                Logger::error("Recovery callback failed for signal " + std::to_string(signal_num));
                return false;
            }
        }
        return false;
    }

    // Cleanup crash recovery system
    static void cleanup()
    {
        // Restore default signal handlers
        signal(SIGSEGV, SIG_DFL);
        signal(SIGBUS, SIG_DFL);
        signal(SIGFPE, SIG_DFL);
        signal(SIGILL, SIG_DFL);
        signal(SIGABRT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);

        custom_handlers_.clear();
        recovery_callbacks_.clear();

        Logger::info("Crash recovery system cleaned up");
    }

    // Shutdown crash recovery system (alias for cleanup)
    static void shutdown()
    {
        cleanup();
    }

private:
    static std::atomic<bool> stack_trace_enabled_;
    static std::atomic<int> max_stack_trace_depth_;
    static std::map<int, SignalHandlerFunc> custom_handlers_;
    static std::map<int, std::function<bool()>> recovery_callbacks_;

    // Set up alternate signal stack for handling stack overflow
    static void setupAlternateSignalStack()
    {
        stack_t ss;
        ss.ss_sp = malloc(SIGSTKSZ);
        if (ss.ss_sp)
        {
            ss.ss_size = SIGSTKSZ;
            ss.ss_flags = 0;
            if (sigaltstack(&ss, nullptr) == 0)
            {
                Logger::debug("Alternate signal stack configured");
            }
            else
            {
                Logger::warn("Failed to configure alternate signal stack");
                free(ss.ss_sp);
            }
        }
        else
        {
            Logger::warn("Failed to allocate alternate signal stack");
        }
    }

    // Signal handler functions
    static void handleSegfault(int sig)
    {
        Logger::error("SIGSEGV detected - segmentation fault");
        if (stack_trace_enabled_.load())
        {
            printStackTrace();
        }

        if (!attemptRecovery(sig))
        {
            signal(sig, SIG_DFL); // Reset to default handler
            raise(sig);           // Re-raise the signal
        }
    }

    static void handleBusError(int sig)
    {
        Logger::error("SIGBUS detected - memory access error");
        if (stack_trace_enabled_.load())
        {
            printStackTrace();
        }

        if (!attemptRecovery(sig))
        {
            signal(sig, SIG_DFL);
            raise(sig);
        }
    }

    static void handleFloatingPoint(int sig)
    {
        Logger::error("SIGFPE detected - floating point error");
        if (stack_trace_enabled_.load())
        {
            printStackTrace();
        }

        if (!attemptRecovery(sig))
        {
            signal(sig, SIG_DFL);
            raise(sig);
        }
    }

    static void handleIllegalInstruction(int sig)
    {
        Logger::error("SIGILL detected - illegal instruction");
        if (stack_trace_enabled_.load())
        {
            printStackTrace();
        }

        if (!attemptRecovery(sig))
        {
            signal(sig, SIG_DFL);
            raise(sig);
        }
    }

    static void handleAbort(int sig)
    {
        Logger::error("SIGABRT detected - abort called");
        if (stack_trace_enabled_.load())
        {
            printStackTrace();
        }

        if (!attemptRecovery(sig))
        {
            signal(sig, SIG_DFL);
            raise(sig);
        }
    }

    static void handleTermination(int sig)
    {
        Logger::info("SIGTERM received - graceful shutdown requested");
        // Allow graceful shutdown
        signal(sig, SIG_DFL);
    }

    static void handleInterrupt(int sig)
    {
        Logger::info("SIGINT received - interrupt requested");
        // Allow graceful shutdown
        signal(sig, SIG_DFL);
    }

    // Generate stack trace
    static std::string generateStackTrace(int max_depth)
    {
        void *callstack[128];
        int frames = backtrace(callstack, std::min(max_depth, 128));
        char **symbols = backtrace_symbols(callstack, frames);

        std::stringstream ss;
        ss << "Stack trace (" << frames << " frames):\n";

        for (int i = 0; i < frames; i++)
        {
            Dl_info info;
            if (dladdr(callstack[i], &info))
            {
                int status;
                char *demangled = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
                if (demangled)
                {
                    ss << "  " << i << ": " << demangled << " (" << info.dli_fname << ")\n";
                    free(demangled);
                }
                else
                {
                    ss << "  " << i << ": " << info.dli_sname << " (" << info.dli_fname << ")\n";
                }
            }
            else
            {
                ss << "  " << i << ": " << symbols[i] << "\n";
            }
        }

        std::string result = ss.str();
        free(symbols);
        return result;
    }

    // Print stack trace to log
    static void printStackTrace()
    {
        std::string stack_trace = generateStackTrace(max_stack_trace_depth_.load());
        Logger::error(stack_trace);
    }

    // Get human-readable signal name
    static std::string getSignalName(int signal_num)
    {
        switch (signal_num)
        {
        case SIGSEGV:
            return "SIGSEGV (Segmentation Fault)";
        case SIGBUS:
            return "SIGBUS (Bus Error)";
        case SIGFPE:
            return "SIGFPE (Floating Point Exception)";
        case SIGILL:
            return "SIGILL (Illegal Instruction)";
        case SIGABRT:
            return "SIGABRT (Abort)";
        case SIGTERM:
            return "SIGTERM (Termination)";
        case SIGINT:
            return "SIGINT (Interrupt)";
        default:
            return "Unknown Signal (" + std::to_string(signal_num) + ")";
        }
    }
};

// Initialize static members
std::atomic<bool> CrashRecovery::stack_trace_enabled_{true};
std::atomic<int> CrashRecovery::max_stack_trace_depth_{64};
std::map<int, CrashRecovery::SignalHandlerFunc> CrashRecovery::custom_handlers_;
std::map<int, std::function<bool()>> CrashRecovery::recovery_callbacks_;
