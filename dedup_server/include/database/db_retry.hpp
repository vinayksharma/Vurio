#pragma once

#include <Poco/Logger.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <exception>
#include <string>
#include <thread>

namespace MediaDedup
{
    /// Bounded retry for SQLite writes that lose the write lock.
    ///
    /// WAL admits exactly one writer, so any long write transaction elsewhere
    /// in the process makes every other writer fail with SQLITE_BUSY once the
    /// per-connection `busy_timeout` (5s, see SessionManager) runs out. Those
    /// failures are not data errors — the row is perfectly writable a moment
    /// later — so swallowing them silently is what turns a contended cache
    /// write into an endless regenerate/fail/regenerate loop.
    namespace DbRetry
    {
        /// True when the exception text names a lock/busy condition. Poco's
        /// SQLite backend surfaces these as "DB locked", "database is locked",
        /// or "database table is locked" depending on the failure point.
        inline bool isBusyError(const std::string &message)
        {
            std::string lowered = message;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return lowered.find("lock") != std::string::npos ||
                   lowered.find("busy") != std::string::npos;
        }

        /// Run `op` (which signals failure by throwing) until it succeeds, the
        /// error turns out not to be lock contention, or the attempts run out.
        /// Returns whether `op` ultimately succeeded; logs the outcome under
        /// `logger_name` describing the operation as `what`.
        template <typename Op>
        bool withBusyRetry(const char *logger_name, const char *what, Op &&op)
        {
            // 5 attempts / ~1.5s of added backoff, each of which already waits
            // out the connection's 5s busy_timeout before it throws.
            constexpr int kMaxAttempts = 5;
            std::chrono::milliseconds delay{100};

            for (int attempt = 1;; ++attempt)
            {
                try
                {
                    op();
                    return true;
                }
                catch (const std::exception &e)
                {
                    const std::string message = e.what();
                    Poco::Logger &logger = Poco::Logger::get(logger_name);

                    if (!isBusyError(message) || attempt >= kMaxAttempts)
                    {
                        logger.error("Exception in " + std::string(what) + ": " + message +
                                     (attempt > 1 ? " (after " + std::to_string(attempt) + " attempts)" : ""));
                        return false;
                    }

                    logger.warning(std::string(what) + " lost the write lock (" + message + "), retrying in " +
                                   std::to_string(delay.count()) + " ms (attempt " + std::to_string(attempt) +
                                   "/" + std::to_string(kMaxAttempts) + ")");
                    std::this_thread::sleep_for(delay);
                    delay *= 2;
                }
            }
        }
    }
}
