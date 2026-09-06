#include "utils/subprocess_runner.hpp"

#include <Poco/Logger.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char **environ;

namespace MediaDedup
{
    namespace
    {
        // Build the child's environment: the parent's, with extra_env entries
        // replacing same-named variables. Returned pointers borrow from
        // `storage`, which must outlive the spawn.
        std::vector<char *> buildEnvp(const std::map<std::string, std::string> &extra_env,
                                      std::vector<std::string> &storage)
        {
            for (char **e = environ; e && *e; ++e)
            {
                const char *eq = std::strchr(*e, '=');
                if (!eq)
                    continue;
                if (extra_env.count(std::string(*e, eq - *e)))
                    continue; // overridden below
                storage.emplace_back(*e);
            }
            for (const auto &[k, v] : extra_env)
                storage.emplace_back(k + "=" + v);

            std::vector<char *> envp;
            envp.reserve(storage.size() + 1);
            for (auto &s : storage)
                envp.push_back(s.data());
            envp.push_back(nullptr);
            return envp;
        }

        // Non-blocking drain of the capture pipe. Returns false once the pipe
        // has reached EOF (all writers gone).
        bool drainPipe(int fd, std::string &out)
        {
            char buf[4096];
            while (true)
            {
                ssize_t n = ::read(fd, buf, sizeof(buf));
                if (n > 0)
                {
                    out.append(buf, static_cast<size_t>(n));
                    continue;
                }
                if (n == 0)
                    return false; // EOF
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    return true; // no data right now, writers still alive
                if (errno == EINTR)
                    continue;
                return false; // read error: treat as closed
            }
        }
    } // namespace

    SubprocessResult runProcessWithTimeout(const std::string &command,
                                           const std::vector<std::string> &args,
                                           int timeout_ms,
                                           const std::map<std::string, std::string> &extra_env,
                                           std::string *capture_stdout)
    {
        SubprocessResult result;
        Poco::Logger &logger = Poco::Logger::get("SubprocessRunner");

        // argv
        std::vector<std::string> argv_storage;
        argv_storage.reserve(args.size() + 1);
        argv_storage.push_back(command);
        for (const auto &a : args)
            argv_storage.push_back(a);
        std::vector<char *> argv;
        argv.reserve(argv_storage.size() + 1);
        for (auto &s : argv_storage)
            argv.push_back(s.data());
        argv.push_back(nullptr);

        // envp
        std::vector<std::string> env_storage;
        std::vector<char *> envp = buildEnvp(extra_env, env_storage);

        // Optional stdout capture pipe.
        int out_pipe[2] = {-1, -1};
        if (capture_stdout)
        {
            capture_stdout->clear();
            if (::pipe(out_pipe) != 0)
            {
                logger.error("pipe() failed for stdout capture: %s", std::string(std::strerror(errno)));
                return result;
            }
        }

        posix_spawnattr_t attr;
        posix_spawnattr_init(&attr);
        // Own process group so a timeout can SIGKILL the child AND anything
        // it spawned (delegate grandchildren) in one shot, without touching
        // the server's group.
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
        posix_spawnattr_setpgroup(&attr, 0);

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        if (capture_stdout)
        {
            posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO);
            posix_spawn_file_actions_addclose(&actions, out_pipe[0]);
            posix_spawn_file_actions_addclose(&actions, out_pipe[1]);
        }

        pid_t pid = -1;
        // spawnp: PATH search for bare command names (absolute paths unaffected).
        int spawn_rc = ::posix_spawnp(&pid, command.c_str(), &actions, &attr,
                                      argv.data(), envp.data());
        posix_spawn_file_actions_destroy(&actions);
        posix_spawnattr_destroy(&attr);

        if (capture_stdout)
        {
            ::close(out_pipe[1]); // parent keeps only the read end
            // Non-blocking so the wait loop can interleave draining.
            int flags = ::fcntl(out_pipe[0], F_GETFL, 0);
            ::fcntl(out_pipe[0], F_SETFL, flags | O_NONBLOCK);
        }

        if (spawn_rc != 0)
        {
            logger.error("posix_spawn failed for %s: %s", command, std::string(std::strerror(spawn_rc)));
            if (capture_stdout)
                ::close(out_pipe[0]);
            return result;
        }

        const bool unbounded = timeout_ms <= 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(unbounded ? 0 : timeout_ms);
        // CC-6: adaptive backoff (1->2->4...->50 ms). The fixed 50 ms poll
        // added ~25 ms mean latency to EVERY spawn — 40-80% overhead on a
        // typical 30-60 ms thumbnail render, on a counted pool slot.
        auto poll_interval = std::chrono::milliseconds(1);
        bool pipe_open = capture_stdout != nullptr;

        while (true)
        {
            if (pipe_open)
                pipe_open = drainPipe(out_pipe[0], *capture_stdout);

            int status = 0;
            pid_t rc = ::waitpid(pid, &status, WNOHANG);
            if (rc == pid)
            {
                result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status)
                                                     : (WIFSIGNALED(status) ? -WTERMSIG(status) : -1);
                break;
            }
            if (rc < 0 && errno != EINTR)
            {
                logger.error("waitpid failed for %s: %s", command, std::string(std::strerror(errno)));
                break;
            }

            if (!unbounded && std::chrono::steady_clock::now() >= deadline)
            {
                logger.error("Subprocess exceeded %d ms timeout, killing group: %s", timeout_ms, command);
                // Kill the whole process group. The child is still a zombie
                // (unreaped) at this point, so the group id cannot have been
                // recycled. Fall back to the single pid if the group kill
                // fails for any reason.
                if (::kill(-pid, SIGKILL) != 0 && errno != ESRCH)
                {
                    logger.warning("killpg failed (%s), killing child pid only", std::string(std::strerror(errno)));
                    ::kill(pid, SIGKILL);
                }
                // Reap the killed child so it does not linger as a zombie.
                int status2 = 0;
                while (::waitpid(pid, &status2, 0) < 0 && errno == EINTR)
                {
                }
                result.timed_out = true;
                break;
            }

            std::this_thread::sleep_for(poll_interval);
            poll_interval = std::min(poll_interval * 2, std::chrono::milliseconds(50));
        }

        if (capture_stdout)
        {
            // Pick up anything buffered after exit. Writers are gone on every
            // path (normal exit: magick waits for its delegates; timeout: the
            // group was SIGKILLed), so this ends at EOF quickly — but bound it
            // anyway rather than trusting an inherited fd to close.
            const auto drain_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (pipe_open && std::chrono::steady_clock::now() < drain_deadline)
            {
                pipe_open = drainPipe(out_pipe[0], *capture_stdout);
                if (pipe_open)
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            ::close(out_pipe[0]);
        }

        return result;
    }

} // namespace MediaDedup
