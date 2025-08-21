#pragma once
#include <functional>
#include <chrono>
#include <thread>
#include <atomic>
#include <string>
#include <future>
#include <libavutil/error.h>
#include "logging/logger.hpp"

class ErrorRecovery
{
public:
    // Retry mechanism with exponential backoff
    template <typename Func, typename... Args>
    static auto retryWithBackoff(Func func, int max_retries, const std::string &operation_name, Args &&...args)
        -> decltype(func(std::forward<Args>(args)...))
    {

        for (int attempt = 0; attempt < max_retries; ++attempt)
        {
            try
            {
                return func(std::forward<Args>(args)...);
            }
            catch (const std::exception &e)
            {
                if (attempt == max_retries - 1)
                {
                    Logger::error("External library call failed after " + std::to_string(max_retries) +
                                  " attempts for operation: " + operation_name + " - " + e.what());
                    throw; // Re-throw on final attempt
                }

                int delay_ms = (1 << attempt) * 100; // Exponential backoff: 100ms, 200ms, 400ms...
                Logger::warn("External library call failed for operation '" + operation_name +
                             "', retrying in " + std::to_string(delay_ms) + "ms (attempt " +
                             std::to_string(attempt + 1) + "/" + std::to_string(max_retries) +
                             "): " + e.what());

                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            }
        }
        throw std::runtime_error("All retry attempts failed for operation: " + operation_name);
    }

    // Retry mechanism for FFmpeg operations that return error codes
    template <typename Func, typename... Args>
    static int retryFFmpegOperation(Func func, int max_retries, const std::string &operation_name, Args &&...args)
    {
        for (int attempt = 0; attempt < max_retries; ++attempt)
        {
            int result = func(std::forward<Args>(args)...);
            if (result >= 0)
            {
                return result; // Success
            }

            if (attempt == max_retries - 1)
            {
                char err_buf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(result, err_buf, AV_ERROR_MAX_STRING_SIZE);
                Logger::error("FFmpeg operation '" + operation_name + "' failed after " +
                              std::to_string(max_retries) + " attempts: " + std::string(err_buf) +
                              " (error code: " + std::to_string(result) + ")");
                return result;
            }

            int delay_ms = (1 << attempt) * 100;
            char err_buf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(result, err_buf, AV_ERROR_MAX_STRING_SIZE);
            Logger::warn("FFmpeg operation '" + operation_name + "' failed, retrying in " +
                         std::to_string(delay_ms) + "ms (attempt " + std::to_string(attempt + 1) +
                         "/" + std::to_string(max_retries) + "): " + std::string(err_buf));

            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }
        return -1; // Should never reach here
    }

    // Circuit breaker pattern for external library calls
    class CircuitBreaker
    {
    private:
        std::atomic<bool> is_open_{false};
        std::atomic<int> failure_count_{0};
        std::chrono::steady_clock::time_point last_failure_time_;
        const int failure_threshold_;
        const std::chrono::seconds timeout_;
        std::string operation_name_;

    public:
        CircuitBreaker(const std::string &operation_name, int threshold = 5, int timeout_seconds = 60)
            : failure_threshold_(threshold), timeout_(timeout_seconds), operation_name_(operation_name) {}

        template <typename Func, typename... Args>
        auto call(Func func, Args &&...args) -> decltype(func(std::forward<Args>(args)...))
        {
            if (is_open_.load())
            {
                if (std::chrono::steady_clock::now() - last_failure_time_ > timeout_)
                {
                    is_open_.store(false);
                    failure_count_.store(0);
                    Logger::info("Circuit breaker closed for operation '" + operation_name_ +
                                 "', retrying external library calls");
                }
                else
                {
                    throw std::runtime_error("Circuit breaker is open for operation '" + operation_name_ +
                                             "' - external library calls are blocked");
                }
            }

            try
            {
                auto result = func(std::forward<Args>(args)...);
                failure_count_.store(0);
                return result;
            }
            catch (...)
            {
                failure_count_.fetch_add(1);
                if (failure_count_.load() >= failure_threshold_)
                {
                    is_open_.store(true);
                    last_failure_time_ = std::chrono::steady_clock::now();
                    Logger::error("Circuit breaker opened for operation '" + operation_name_ +
                                  "' due to repeated failures");
                }
                throw;
            }
        }

        bool isOpen() const
        {
            return is_open_.load();
        }
        int getFailureCount() const
        {
            return failure_count_.load();
        }
        std::string getOperationName() const
        {
            return operation_name_;
        }
    };

    // Timeout wrapper for external library calls
    template <typename Func, typename... Args>
    static auto callWithTimeout(Func func, int timeout_ms, const std::string &operation_name, Args &&...args)
        -> decltype(func(std::forward<Args>(args)...))
    {
        std::atomic<bool> completed{false};
        std::atomic<bool> timed_out{false};
        std::exception_ptr exception_ptr;

        auto future_result = std::async(std::launch::async, [&]()
                                        {
            try {
                auto result = func(std::forward<Args>(args)...);
                completed.store(true);
                return result;
            } catch (...) {
                exception_ptr = std::current_exception();
                completed.store(true);
                throw;
            } });

        auto start_time = std::chrono::steady_clock::now();
        while (!completed.load())
        {
            auto elapsed = std::chrono::steady_clock::now() - start_time;
            if (elapsed > std::chrono::milliseconds(timeout_ms))
            {
                timed_out.store(true);
                Logger::error("Operation '" + operation_name + "' timed out after " +
                              std::to_string(timeout_ms) + "ms");
                throw std::runtime_error("Operation '" + operation_name + "' timed out");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        if (exception_ptr)
        {
            std::rethrow_exception(exception_ptr);
        }

        return future_result.get();
    }

    // Graceful degradation for non-critical operations
    template <typename Func, typename FallbackFunc, typename... Args>
    static auto callWithFallback(Func primary_func, FallbackFunc fallback_func,
                                 const std::string &operation_name, Args &&...args)
        -> decltype(primary_func(std::forward<Args>(args)...))
    {
        try
        {
            return primary_func(std::forward<Args>(args)...);
        }
        catch (const std::exception &e)
        {
            Logger::warn("Primary operation '" + operation_name + "' failed, using fallback: " + e.what());
            try
            {
                return fallback_func(std::forward<Args>(args)...);
            }
            catch (const std::exception &fallback_e)
            {
                Logger::error("Both primary and fallback operations failed for '" + operation_name +
                              "': " + fallback_e.what());
                throw;
            }
        }
    }
};
